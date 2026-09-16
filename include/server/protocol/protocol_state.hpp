#ifndef PROTOCOL_PROTOCOL_STATE_HPP
#define PROTOCOL_PROTOCOL_STATE_HPP

namespace protocol
{

enum class WireProtocol
{
    Undetermined,
    Json,
    Protobuf,
    Binary
};

// 每个 TCP 连接独占一份状态。自动检测成功后协议固定；只有显式协商可切换。
class ProtocolState final
{
public:
    WireProtocol selected() const noexcept { return _selected; }
    bool determined() const noexcept
    {
        return _selected != WireProtocol::Undetermined;
    }

    bool selectDetected(WireProtocol protocol) noexcept
    {
        if (_selected == WireProtocol::Undetermined)
        {
            _selected = protocol;
        }
        return _selected == protocol;
    }

    void selectAfterNegotiation(WireProtocol protocol) noexcept
    {
        _selected = protocol;
    }

private:
    WireProtocol _selected = WireProtocol::Undetermined;
};

} // namespace protocol

#endif
