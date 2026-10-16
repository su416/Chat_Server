# Chat_Server — C++ 分布式实时消息网关

这是一个基于 C++17 和 Muduo 的多节点实时消息服务。项目从单机聊天室逐步演进为具备 Session 所有权、JSON/Protobuf 双协议、Redis Presence、Redis Streams 跨节点路由、客户端 ACK/重试、MySQL durable pending、离线收件箱与有界背压的完整工程。

系统提供 **bounded at-least-once delivery**，不声称 Exactly Once，也不把应用 ACK 等同于已读回执。

## Key Features

- Muduo Reactor 网络模型，连接 context 只在所属 I/O loop 使用
- JSON 与 Protobuf framing、粘包/半包处理和连接级协议状态
- `SessionManager` 管理本节点用户与连接生命周期
- Redis Presence：`user -> owner node`，带租约和 compare-and-delete
- Redis Streams：Consumer Group、PEL、XAUTOCLAIM、routeId 去重
- 两层身份：跨节点 `routeId`，目标节点到客户端 `deliveryId`
- MySQL `pending_delivery`：重启恢复、离线 `WAITING_SESSION`、同 ID 重试
- 客户端 ACK、超时重试和客户端 deliveryId 去重
- 有界 DB WorkerPool、优先级/保留容量、deferred ACK
- Runtime Pending 条数/字节限制、Stream 准入、慢消费者保护
- CTest、真实双节点集成、故障注入与可复现 benchmark

## Architecture

```text
JSON / Protobuf clients
        │
   ┌────┴───────────────┐
   ▼                    ▼
ChatServer A        ChatServer B
Session/Router      Session/Router
PendingManager      PendingManager
   │                    │
   └──── Redis ─────────┘
        Presence + per-node Streams
              │
            MySQL
      history + pending_delivery
```

完整组件、线程和消息流程见 [Architecture](docs/architecture.md)；可靠性与故障语义见 [Reliability](docs/reliability.md)。

## Message Lifecycle

```text
request
  -> durable acceptance
  -> WAITING_SESSION / PENDING
  -> client receives deliveryId
  -> client ACK
  -> ACKED
```

- **Accepted**：对应 durable storage commit 成功。
- **Delivered**：客户端收到消息，尚不代表服务端已持久处理 ACK。
- **ACKed**：客户端 ACK 已在 durable store 完成。
- **Read**：项目没有实现独立已读回执语义。

## Project Structure

```text
conf/       示例和双节点配置
docs/       架构、可靠性、部署、基准文档
include/    服务端公共头文件
proto/      Protobuf schema
qt_client/  可选 Qt6 GUI 客户端
script/     构建、数据库、运行、测试和故障注入入口
sql/        fresh-install schema
src/        ChatServer 与命令行客户端
tests/      CTest 单元/组件测试
```

## Dependencies

已验证环境：Ubuntu 24.04.4、GCC 13.3、CMake、MySQL 8、Redis 7、Protobuf 3.21、Muduo、hiredis、OpenSSL、nlohmann/json；Python 3 用于集成测试。Qt6 仅 GUI 客户端需要。

```bash
sudo apt update
sudo apt install -y build-essential cmake git libboost-dev \
  default-libmysqlclient-dev libhiredis-dev libprotobuf-dev \
  protobuf-compiler nlohmann-json3-dev libssl-dev \
  mysql-client redis-tools python3
```

Muduo 请安装到系统可搜索路径；Dockerfile 会自行构建 Muduo。

## Build

```bash
./script/build.sh
ctest --test-dir build --output-on-failure
```

构建脚本会清理 `build/`，生成 C++ Protobuf binding，再构建 `bin/ChatServer` 与 `bin/ChatClient`。

## Configuration

复制并按环境修改示例：

```bash
cp conf/chatserver.example.json conf/chatserver.local.json
```

关键项包括监听地址、`node_id`、MySQL/Redis、DB queue、Pending 上限、ACK timeout/retry、offline claim、Stream soft limit 和 socket output watermarks。容器可使用 `CHAT_SERVER_*`、`CHAT_NODE_ID`、`CHAT_MYSQL_*`、`CHAT_REDIS_*` 覆盖连接参数，密码无需写入镜像。

普通单聊/群聊可启用 per-user + Gateway global Token Bucket，控制消息绕过聊天配额；目标 Stream 到达高水位时明确返回 `TARGET_OVERLOADED`。配置兼容、错误语义和验证方法见 [流量治理](docs/rate_limiting.md)。

提交到仓库的 `chat_password`、`root_password` 只是本地演示占位值，不能用于真实部署。

## Database Initialization

```bash
export CHAT_DB_ADMIN_PASSWORD='your-root-password'
python3 script/init_db.py --config conf/chatserver.local.json
```

该入口会创建数据库/应用用户、应用 fresh schema，并补齐 Phase 7 的 nullable column 和索引；可重复运行。旧环境仍可单独使用 `migrate_phase6.py`、`migrate_phase7.py`。

Redis 不需要手动初始化，节点注册、Presence key、Stream 和 Consumer Group 由服务端创建。

## Run Single Node

```bash
./script/run_server.sh conf/chatserver.local.json
```

JSON 客户端：

```bash
./script/run_client.sh 127.0.0.1 6000
```

Protobuf 客户端：

```bash
./bin/ChatClient 127.0.0.1 6000 --proto
```

## Run Two Nodes

`conf/chatserver-node-a.json` 和 `conf/chatserver-node-b.json` 分别监听 6000/6001，并使用不同 `node_id`：

```bash
./script/run_cluster.sh
python3 script/demo_two_node.py
./script/stop_cluster.sh
```

## Docker Compose

```bash
cp .env.example .env
docker compose build
docker compose up -d
docker compose ps
python3 script/demo_two_node.py
docker compose down -v
```

Compose 从 fresh MySQL volume 启动 MySQL、Redis、Node A（6000）和 Node B（6001），并等待依赖 healthcheck。详细说明见 [Deployment](docs/deployment.md)。

## Qt Client

```bash
QT_ROOT=/path/to/Qt/6.x/gcc_64 ./qt_client/build.sh
./qt_client/build/bin/ChatClient
```

若 Qt6 已安装到标准路径，可省略 `QT_ROOT`。

## Testing

快速完整可靠性回归（要求本机 MySQL/Redis 已就绪）：

```bash
./script/test_all.sh
```

包含 Phase 3–7。较慢的 500/1000 离线积压、Phase 8 fault injection 和压力测试：

```bash
./script/test_all.sh --full
```

Python binding 可独立生成：

```bash
./script/gen_python_proto.sh
```

## Benchmark

Phase 8 最终单 VM 样本：

| Workload | Protocol | Accepted | Rejected | Unique QPS | P50/P95/P99 |
|---|---|---:|---:|---:|---|
| 10×100 | JSON | 1000 | 0 | 449.8 | 619/1264/1308 ms |
| 20×100 | JSON | 2000 | 0 | 774.9 | 1461/1701/1740 ms |
| 50×100 | JSON | 4516 | 484 | 715.1 | 2993/4928/5250 ms |
| 10×100 | Protobuf | 1000 | 0 | 472.8 | 681/1231/1256 ms |

50×100 的 484 条在 durable acceptance 前明确拒绝，不是静默丢失。完整条件与限制见 [Benchmark](docs/benchmark.md)。

## Failure Injection

- `phase6_phase3_regression.py`：ACK 丢失、同 deliveryId 重试和去重
- `phase5_stream_integration.py`：kill -9、PEL/XAUTOCLAIM、Redis reconnect
- `phase6_durable_pending_integration.py`：persist/send/ACK crash windows
- `phase7_offline_inbox_integration.py`：离线、迁移、重启和容量恢复
- `phase8_backpressure_integration.py`：DB 饱和、慢消费者、Stream 过载

## Evolution Roadmap

| Phase | Problem | Decision |
|---:|---|---|
| 1 | user/connection ownership | SessionManager |
| 2 | protocol/business coupling | Codec + Envelope + Router |
| 3 | TCP send 不等于确认送达 | deliveryId + ACK + retry |
| 4 | 用户位于其他节点 | Presence + owner node routing |
| 5 | Pub/Sub 不可恢复 | Redis Streams + PEL/XAUTOCLAIM |
| 6 | 进程重启丢失 Pending | MySQL durable pending |
| 7 | 在线/离线生命周期分裂 | unified durable inbox |
| 8 | 过载导致资源增长和 ACK 饥饿 | bounded backpressure |
| 9 | 难以复现部署和展示 | packaging + verified deployment |

## Known Limitations

- At-Least-Once，不是 Exactly Once；客户端必须按 deliveryId 去重。
- ACK 不是 read receipt。
- Redis/MySQL 是外部依赖，部署持久性取决于它们的配置。
- 少量 legacy MySQL/Redis 调用仍可能同步执行于 I/O 路径。
- 群聊尚未实现 durable fan-out；Binary 路径未完全统一到 reliable delivery。
- Benchmark 仅为 Redis/MySQL 同机的单 VM、debug-style、单样本结果；没有内核调优或生产网络延迟。
- Qt GUI 当前连接地址仍固定为 `127.0.0.1:6000`；可配置客户端地址属于后续 UI 配置工作。
- 未提供 Kubernetes、service mesh 或完整 Prometheus/OpenTelemetry 栈。

## License

MIT
