# 实时消息网关演进清单

只有在代码检查或实际运行记录提供证据后，才能勾选任务。不得提前勾选未来
阶段的工作。

## Phase 0 — 基线

### 审计

- [x] 确认 Git 状态已知且可解释。
- [x] 记录当前分支和提交。
- [x] 记录源码目录概览。
- [x] 记录实际构建命令。
- [x] 记录验证环境中的依赖版本。
- [x] 记录现有测试。
- [x] 记录现有基准测试脚本。
- [x] 记录当前网络线程模型。
- [x] 记录当前用户与连接映射。
- [x] 记录当前 JSON 分帧行为。
- [x] 记录当前 Protobuf 分帧行为。
- [x] 记录当前 Redis 转发路径。
- [x] 记录当前异步 DB 路径。
- [x] 识别 Reactor 线程上的阻塞调用。
- [x] 识别无界队列、映射和缓冲区。

### 验证

- [x] 修复已记录的预存回归后，当前源码构建通过。
- [x] 现有自动化测试全部通过（5/5）。
- [x] 基础注册、登录和单聊冒烟通过（5/5 条消息）。
- [x] 已尝试代表性基准；初始化超时已记录，未编造数据。
- [x] 已记录环境。
- [x] 已创建 `docs/evolution/phase_00_baseline.md`。
- [x] 已产生 Phase 0 基线提交。

## Phase 1 — SessionManager

- [ ] 定义 Session 与 `TcpConnection` 的职责和所有权。
- [ ] 定义重复登录及陈旧连接策略。
- [ ] 添加 ADR、实现、不变量和单元/集成测试。
- [ ] 创建 `docs/evolution/phase_01_session.md`。

## Phase 2 — 内部消息信封与协议边界

- [ ] 定义协议无关的消息信封和显式解码结果。
- [ ] 定义固定协议选择和最大帧大小。
- [ ] 通过测试保持 JSON 和 Protobuf 兼容性。
- [ ] 添加 ADR 和 `docs/evolution/phase_02_message_envelope.md`。

## Phase 3 — 消息 ID 与 ACK 状态

- [ ] 定义并测试稳定消息 ID。
- [ ] 定义有容量与 TTL 限制的 pending 状态及幂等 ACK 语义。
- [ ] 添加 ADR 和 `docs/evolution/phase_03_message_id_ack.md`。

## Phase 4 — 有界重试与至少一次投递

- [ ] 定义重试间隔、最大次数、过期和去重契约。
- [ ] 测试 ACK、重试、过期、迟到 ACK、重复 ACK 和重连。
- [ ] 添加 ADR 和 `docs/evolution/phase_04_reliable_delivery.md`。

## Phase 5 — 背压

- [ ] 限制 DB 队列容量并处理提交失败。
- [ ] 定义出站高水位和慢消费者策略。
- [ ] 测试饱和、恢复和负载下关闭。
- [ ] 添加 ADR 和 `docs/evolution/phase_05_backpressure.md`。

## Phase 6 — 可观测性

- [ ] 添加线程安全的投递、队列、会话、Redis 和延迟指标。
- [ ] 增加结构化关联信息，但不记录私密消息正文。
- [ ] 添加并发测试和 `docs/evolution/phase_06_observability.md`。

## Phase 7 — Presence 与显式跨网关路由

- [ ] 定义网关身份、Presence 结构、TTL 和降级行为。
- [ ] 测试本地、远端、过期、节点消失和 Redis 重启路径。
- [ ] 添加 ADR 和 `docs/evolution/phase_07_presence.md`。

## Phase 8 — 故障验证、基准与项目定位

- [ ] 执行已定义的 Redis、MySQL、ACK、队列、帧和慢客户端故障测试。
- [ ] 使用完整复现信息比较基础路径与可靠路径。
- [ ] 更新最终架构、可靠性、背压、故障和基准文档。
- [ ] 创建 `docs/evolution/phase_08_validation.md`。
