#include "chatserver.hpp"
#include "json.hpp"
#include "chatservice.hpp"
#include "protocol/message_decoder.hpp"
#include "protocol/message_encoder.hpp"
#include "protocol/protocol_state.hpp"
#include "config.hpp"

#include <boost/any.hpp>
#include <muduo/base/Logging.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <algorithm>

using namespace std;
using namespace placeholders;
using json = nlohmann::json;

std::mutex ChatServer::_flowMutex;
std::unordered_map<const TcpConnection*, ChatServer::FlowState> ChatServer::_connectionFlow;
std::atomic<std::uint64_t> ChatServer::_activeConnections{0},
    ChatServer::_slowConsumers{0}, ChatServer::_highwaterEvents{0},
    ChatServer::_hardDisconnects{0}, ChatServer::_queuedPeakBytes{0},
    ChatServer::_outputPeakBytes{0};

void ChatServer::markSlow(const TcpConnectionPtr& conn, bool slow) {
    std::lock_guard<std::mutex> lock(_flowMutex);
    auto it = _connectionFlow.find(conn.get());
    if (it == _connectionFlow.end() || it->second.slow == slow) return;
    it->second.slow = slow;
    if (slow) ++_slowConsumers;
    else --_slowConsumers;
}

ChatServer::ReserveOutcome ChatServer::reserveQueued(
    const TcpConnectionPtr& conn, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(_flowMutex);
    auto it = _connectionFlow.find(conn.get());
    if (it == _connectionFlow.end() || it->second.closeQueued)
        return ReserveOutcome::AlreadyClosing;
    const auto hard = static_cast<std::size_t>(
        Config::instance().getOutputHardLimitBytes());
    if (bytes > hard || it->second.queuedBytes > hard - bytes) {
        it->second.closeQueued = true;
        return ReserveOutcome::Close;
    }
    it->second.queuedBytes += bytes;
    auto peak = _queuedPeakBytes.load();
    while (peak < it->second.queuedBytes &&
           !_queuedPeakBytes.compare_exchange_weak(peak, it->second.queuedBytes)) {}
    return ReserveOutcome::Accepted;
}

void ChatServer::releaseQueued(const TcpConnectionPtr& conn, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(_flowMutex);
    auto it = _connectionFlow.find(conn.get());
    if (it != _connectionFlow.end())
        it->second.queuedBytes -= std::min(bytes, it->second.queuedBytes);
}

ChatServer::OutputMetrics ChatServer::outputMetrics() {
    return {_activeConnections.load(), _slowConsumers.load(),
            _highwaterEvents.load(), _hardDisconnects.load(),
            _queuedPeakBytes.load(), _outputPeakBytes.load()};
}

bool ChatServer::isBackpressured(const TcpConnectionPtr& conn) {
    // Called only on the target connection's I/O loop.
    return conn && conn->outputBuffer()->readableBytes() >=
        static_cast<std::size_t>(Config::instance().getOutputHighWatermarkBytes());
}

void ChatServer::sendResponse(const TcpConnectionPtr& conn,
                              const json& response,
                              bool forceJson)
{
    try
    {
        if (!response.is_object())
        {
            throw std::invalid_argument("outbound response must be JSON object");
        }
        gateway::MessageEnvelope message(response.value("msgid", 0), response,
                                         protocol::WireProtocol::Undetermined);
        sendResponse(conn, message, forceJson);
    }
    catch (const std::exception& error)
    {
        LOG_WARN << "unable to encode client response: " << error.what();
        conn->shutdown();
    }
}

void ChatServer::sendResponse(const TcpConnectionPtr& conn,
                              const gateway::MessageEnvelope& response,
                              bool forceJson)
{
    if (!conn) return;
    // Redis callback、别的连接的 I/O loop、主 loop 定时器都可能调用本入口。
    // 只在目标连接的 EventLoop 读取 ProtocolState，并且不延长断线连接寿命。
    std::weak_ptr<TcpConnection> weakConnection = conn;
    const bool crossThread = conn->getLoop() !=
        muduo::net::EventLoop::getEventLoopOfCurrentThread();
    // Internal JSON is immutable here; this upper-budget estimate also works
    // when a test config uses a hard limit below the maximum accepted frame.
    const auto queuedReservation = crossThread
        ? response.payload().dump().size() + 256 : std::size_t{0};
    if (crossThread) {
        const auto outcome = reserveQueued(conn, queuedReservation);
        if (outcome != ReserveOutcome::Accepted) {
            if (outcome == ReserveOutcome::Close) {
                ++_hardDisconnects;
                conn->getLoop()->queueInLoop([weakConnection]() {
                    if (auto target = weakConnection.lock()) target->forceClose();
                });
            }
            return;
        }
    }
    conn->getLoop()->runInLoop([weakConnection, response, forceJson,
                                crossThread, queuedReservation]() {
        auto target = weakConnection.lock();
        if (crossThread && target) releaseQueued(target, queuedReservation);
        if (!target || !target->connected()) return;
        try
        {
            protocol::WireProtocol selected = protocol::WireProtocol::Json;
            if (!forceJson)
            {
                const auto& context = target->getContext();
                if (const auto* state =
                        boost::any_cast<protocol::ProtocolState>(&context))
                {
                    selected = state->selected();
                }
            }
            const auto frame = protocol::MessageEncoder::encode(response, selected);
            const auto buffered = target->outputBuffer()->readableBytes();
            const auto hard = static_cast<std::size_t>(
                Config::instance().getOutputHardLimitBytes());
            if (frame.size() > hard || buffered > hard - frame.size()) {
                ++_hardDisconnects;
                LOG_WARN << "OUTPUT_HARD_LIMIT connection=" << target->name()
                         << " buffered=" << buffered << " next=" << frame.size();
                target->forceClose(); // durable row stays in MySQL for reconnect
                return;
            }
            target->send(frame);
        }
        catch (const std::exception& error)
        {
            LOG_WARN << "unable to encode client response: " << error.what();
            target->shutdown();
        }
    });
}

// 初始化聊天服务器对象
ChatServer::ChatServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg,
                       int threadNum)
    : _server(loop, listenAddr, nameArg),
      _loop(loop),
      _router([](int messageType) {
          return ChatService::instance()->findHandler(messageType);
      })
{
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, _1));
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, _1, _2, _3));
    _server.setThreadNum(threadNum);
}

void ChatServer::start()
{
    _server.start();
}

void ChatServer::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        conn->setContext(protocol::ProtocolState{});
        {
            std::lock_guard<std::mutex> lock(_flowMutex);
            _connectionFlow.emplace(conn.get(), FlowState{});
        }
        ++_activeConnections;
        const auto high = static_cast<std::size_t>(
            Config::instance().getOutputHighWatermarkBytes());
        std::weak_ptr<TcpConnection> weak = conn;
        conn->setHighWaterMarkCallback([weak, high](const TcpConnectionPtr& target,
                                                    std::size_t bytes) {
            ++_highwaterEvents;
            auto peak = _outputPeakBytes.load();
            while (peak < bytes && !_outputPeakBytes.compare_exchange_weak(peak, bytes)) {}
            markSlow(target, true);
            LOG_WARN << "OUTPUT_HIGH_WATER connection=" << target->name()
                     << " bytes=" << bytes;
            target->getLoop()->runAfter(2.0, [weak, high]() {
                auto current = weak.lock();
                if (current && current->connected()) {
                    if (current->outputBuffer()->readableBytes() >= high / 2) {
                        ++_hardDisconnects;
                        LOG_WARN << "SLOW_CONSUMER_DISCONNECT connection=" << current->name();
                        current->forceClose();
                    } else markSlow(current, false);
                }
            });
        }, high);
        conn->setWriteCompleteCallback([](const TcpConnectionPtr& target) {
            markSlow(target, false);
        });
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(_flowMutex);
            auto it = _connectionFlow.find(conn.get());
            if (it != _connectionFlow.end()) {
                if (it->second.slow) --_slowConsumers;
                _connectionFlow.erase(it);
            }
        }
        --_activeConnections;
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

void ChatServer::onMessage(const TcpConnectionPtr &conn,
                           Buffer *buffer,
                           Timestamp time)
{
    while (buffer->readableBytes() > 0)
    {
        protocol::ProtocolState state;
        const auto& context = conn->getContext();
        if (const auto* stored =
                boost::any_cast<protocol::ProtocolState>(&context))
        {
            state = *stored;
        }

        auto decoded = protocol::MessageDecoder::decode(buffer, state);
        if (decoded.status == protocol::DecodeStatus::Incomplete)
        {
            break;
        }
        if (decoded.status == protocol::DecodeStatus::Error)
        {
            LOG_WARN << "protocol decode rejected connection: "
                     << decoded.detail;
            buffer->retrieveAll();
            conn->shutdown();
            return;
        }

        conn->setContext(state);
        const auto dispatch = _router.dispatch(conn, *decoded.message, time);
        if (dispatch != gateway::DispatchResult::Handled)
        {
            LOG_WARN << "message dispatch rejected type="
                     << decoded.message->messageType();
            buffer->retrieveAll();
            conn->shutdown();
            return;
        }
    }
}
