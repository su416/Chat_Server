# Phase 00 — 冻结并测量现有基线

- 分支：`evolution/realtime-gateway`
- 起始提交：`c00212d2df267c72eb0cd49c3846585d1e717832`
- 结束提交：包含本文档的 Phase 0 提交
- 日期：2026-09-02（Asia/Hong_Kong）
- 构建类型：未显式设置（`CMAKE_BUILD_TYPE` 为空；根 CMake 全局添加 `-g`）
- 验证环境：Ubuntu 24.04.4 LTS 虚拟机，6 vCPU，7.7 GiB 内存

## 1. 背景

当前仓库是一个基于 C++17 和 Muduo 的集群聊天服务器，已经具备 JSON、
Binary 以及部分 Protobuf 接入能力，并实现登录与在线状态、单聊、群聊、
MySQL 离线消息和历史消息持久化、Redis Pub/Sub 跨节点转发、Qt 客户端及
Python 压测脚本。

Phase 0 的任务是冻结、验证并记录这一现有实现，不引入网关抽象、Session、
ACK、重试或背压等后续阶段功能。

## 2. 观察到的问题

仓库已有性能和协议演进文档，但缺少一份与当前提交绑定、可复现的基线记录。
检查还发现当前提交在最小修复前无法构建：

- `password_utils.hpp` 中的 `PasswordUtils::hashPassword()` 和
  `verifyPassword()` 声明被意外删除；
- `password_utils.cpp` 中混入字符 `8`，造成语法错误；
- 虚拟机第一次成功的 5/5 测试来自旧提交 `15ff1c5`，而非当前提交
  `c00212d`，因此不能作为当前基线证据。

两个密码工具文件已恢复到 `15ff1c5` 的已知良好内容。该修复没有改变密码
算法或运行行为。

## 3. 基线

### 3.1 Git 状态

本地仓库最初位于 `dev` 分支的 `c00212d`。该提交比 Gitee 跟踪分支
`origin/dev` 超前 3 个提交，而 GitHub 跟踪分支 `github/dev` 同样指向
`c00212d`。工作区当时已有用户要求的 `.gitignore` 修改。

随后从当前提交创建了独立分支 `evolution/realtime-gateway`，没有重写历史。
Linux 虚拟机从 GitHub 重新克隆了相同提交，并在验证时仅包含上述两个密码
文件的最小修复。

### 3.2 实际构建与测试命令

```bash
git restore \
  --source=15ff1c5ce1d73941660f31ffdfcff95c654e3485 \
  -- include/server/utils/password_utils.hpp \
     src/server/utils/password_utils.cpp

./script/build.sh
ctest --test-dir build --output-on-failure
```

`script/build.sh` 会删除并重建 `build/`，生成 Protobuf C++ 源文件，运行
CMake，再通过 `make -j$(nproc)` 编译。

### 3.3 构建结果

```text
[100%] Linking CXX executable /home/ys/Desktop/chat-server-phase0/bin/ChatServer
[100%] Built target ChatServer
-rwxrwxr-x ... ./bin/ChatServer
```

结论：完成已记录的最小修复后，干净构建通过。

### 3.4 自动化测试结果

```text
1/5 test_rate_limiter ......... Passed
2/5 test_jsoncodec ............ Passed
3/5 test_protocodec ........... Passed
4/5 test_password_utils ....... Passed
5/5 test_friendlistmodel ...... Passed

100% tests passed, 0 tests failed out of 5
Total Test time (real) = 0.56 sec
```

### 3.5 源码目录概览

| 目录 | 作用 | 检查到的文件数 |
|---|---|---:|
| `include/` | 公共头文件和服务端头文件 | 23 |
| `src/` | 服务端和命令行客户端实现 | 22 |
| `tests/` | CTest/GoogleTest 测试 | 6 |
| `script/` | 构建、生成、压测和 perf 脚本 | 10 |
| `proto/` | Protobuf 协议定义 | 1 |
| `qt_client/` | Qt 桌面客户端 | 30 |

仓库中的 `docs/api.md` 和 `docs/architecture.md` 目前为空。有效的历史架构
资料分散在 README、架构与性能报告、测试记录和协议演进文档中。

## 4. 假设

如果先冻结真实实现，并明确记录其失败场景和测量限制，后续阶段就可以一次只
处理一个有证据支持的问题，并与真实基线比较，而不是依赖 README 中的历史
宣传数据。

## 5. 考虑过的替代方案

1. 直接采用 `15ff1c5` 的成功测试结果：拒绝，因为它不是当前源码版本。
2. 将仓库回退到 `15ff1c5`：拒绝，因为这会丢弃后续历史，违背不重写历史的
   原则。
3. 只修复密码声明和语法损坏：采用；修复后的两个文件与已知良好版本一致。
4. 在 Phase 0 中继续修复压测超时和关闭问题：拒绝；它们应作为基线发现，
   由后续明确范围的工作处理。

## 6. 决策

以 `c00212d` 作为起始版本，只应用两文件构建修复，同时记录修复前后的验证
结果。完成基线文档后停止，不开展架构改造。

Phase 0 没有作出实质性架构决策，因此没有创建 ADR。

## 7. 变更前架构

```text
JSON / Binary / Protobuf 客户端
              |
              v
Muduo TcpServer（主循环 + 4 个工作循环）
              |
       ChatServer::onMessage
    首字节协议检测与消息分帧
              |
              v
ChatService 单例 + handler 映射
  |-- user_id -> shared TcpConnection 映射
  |-- 同步 Model 查询和状态更新
  |-- 无界异步 DB 写任务队列
  |-- Redis Pub/Sub 转发
  |-- 令牌桶限流
              |
      +-------+--------+
      v                v
 MySQL 连接池       Redis Pub/Sub
```

## 8. 变更后架构

架构不变。Phase 0 只恢复可构建状态并增加基线文档。

## 9. 实施内容

### 9.1 新增文件

- `docs/evolution/phase_00_baseline.md`
- `docs/PHASE_TASKS.md`

### 9.2 修改文件

- `.gitignore`：忽略仓库根目录下的本地 `codex.md/`。
- `include/server/utils/password_utils.hpp`：恢复两个已有函数声明。
- `src/server/utils/password_utils.cpp`：删除误入的字符 `8`。

### 9.3 重要实现细节

- 密码工具修复与提交 `15ff1c5` 的版本一致，不是新功能实现。
- 未引入 SessionManager、内部消息信封、ACK、重试、队列容量限制或指标系统。

## 10. 并发与生命周期分析

### 10.1 网络与连接状态

- Muduo 持有 `TcpServer` 并调用连接与消息回调。
- 配置使用 4 个工作线程，遵循 one loop per thread 模型。
- `ChatService` 是进程生命周期单例。
- `_userConnMap` 以用户 ID 为键，保存强引用 `TcpConnectionPtr`，由
  `shared_mutex` 保护。
- 登录、注销和断连使用独占锁修改映射；消息路由使用共享锁读取。
- 部分路径会先释放连接映射锁再执行网络或数据库操作，但 Redis 重订阅回调
  会在持有共享锁遍历映射时调用 Redis。

### 10.2 协议状态

- 新连接默认使用 JSON。
- `onMessage()` 对每一帧都通过首字节重新猜测协议，并覆盖连接 context，
  因此协商后的协议状态并非权威状态。
- JSON 使用 NUL 结尾；Protobuf 使用 4 字节大端长度；Binary 使用
  `0xFF`、1 字节消息 ID 和 2 字节长度。
- Codec 支持半包和同一缓冲区中的多帧。
- 所有 Codec 都没有最大帧限制。缺少 NUL 的 JSON 可以无限累积；恶意
  Protobuf 长度也可能让缓冲区无限等待。

### 10.3 数据库任务

- MySQL 连接池包含 4 个连接，默认获取路径在连接耗尽时无限等待。
- 单聊和群聊历史写入会提交给独立的 4 线程 DB 线程池。
- DB 任务队列是无界 `std::queue`，提交没有返回值，也没有饱和策略。
- `DBThreadPool::shutdown()` 会将 `_running` 设置为 false，而 worker 使用
  `while (_running)`。因此 worker 可能在队列仍有任务时退出，关闭过程不保证
  完整排空任务。

### 10.4 Redis 生命周期

- Redis 使用同步 hiredis context。
- 订阅观察线程以 `detach` 方式运行，并通过引用捕获 `Redis` 对象，没有明确
  的停止与 join 生命周期。
- 连接断开后以 3 秒间隔无限重连。
- Redis Pub/Sub 不持久化消息，不能据此声称可靠或持久投递。

## 11. 故障模式

| 故障 | 当前行为或观察结果 | 验证方式 |
|---|---|---|
| 当前版本修复前构建 | 密码工具编译错误 | 实际构建 |
| JSON/Protobuf 半包 | 保留缓冲区并等待完整数据 | 单元测试 |
| JSON/Protobuf 多帧 | 每次提取一帧并继续处理 | 单元测试 |
| 超大帧或缺少分隔符 | 没有配置容量上限 | 代码检查 |
| MySQL 连接池耗尽 | Reactor 或 DB worker 可能无限等待 | 代码检查 |
| DB 队列过载 | 队列无界增长 | 代码检查 |
| Redis publish 失败 | 单聊路径降级为离线数据库存储 | 代码检查 |
| Redis 订阅连接断开 | detach 线程每 3 秒重试 | 代码检查 |
| c=10 注册阶段 | 注册响应超时 | 实际压测 |
| 冒烟测试后 SIGINT | 10 秒内未退出，随后使用 TERM | 实际观察 |

## 12. 测试

### 12.1 自动化测试

修复后的当前版本通过全部 5 个 CTest 测试，总耗时 0.56 秒。CTest 中尚未
注册服务端集成测试。

### 12.2 冒烟测试

命令：

```python
from script.bench_onechat import run_online_to_online
run_online_to_online(1, 5)
```

结果：

```text
场景：online_to_online
并发：1
消息总数：5
成功：5
失败：0
接收端错误：0
发送端错误：0
耗时：0.073 s
QPS：68.4
预热后延迟样本：4
P50：0.54 ms
P95：0.57 ms
P99：0.57 ms
```

该结果验证了注册、登录、本地在线转发、JSON NUL 分帧以及小规模异步历史
写入路径。它没有验证群聊、离线投递、跨节点 Redis 或 Protobuf 端到端路径。

## 13. 基准测试与故障注入

### 13.1 环境

- CPU：6 个虚拟 CPU，宿主型号报告为 13th Gen Intel Core i9-13900HX
- 内存：7.7 GiB；Swap 3.8 GiB
- 操作系统：Ubuntu 24.04.4 LTS；Linux 7.0.0-28-generic x86_64
- 编译器：GCC 13.3.0
- CMake：3.28.3
- Protobuf：3.21.12
- MySQL 客户端/服务端包版本：8.0.46
- Redis Server：7.0.15
- Python：3.12.3
- 构建：没有显式设置 `CMAKE_BUILD_TYPE`，全局添加 `-g`
- 提交：`c00212d` 加已记录的两文件构建修复
- 环境采集时间：2026-09-02T01:38:20+08:00

### 13.2 代表性基准尝试

参数：

- 场景：JSON 在线到在线单聊
- 并发：10 对客户端
- 每对消息数：50
- 计划请求数：500
- 预热：每对前 10% 消息不计入延迟

结果：无效，不能作为性能结果。负载阶段尚未开始，注册
`bench1c_1788287539_c10_oto_ur1` 时已经超时：

```text
RuntimeError: register failed for ..._ur1: None
```

由于工作负载没有开始，本轮不报告 P50、P95、P99、吞吐量或投递成功率。
这个失败本身属于基线证据。此前的小规模冒烟测试成功，但之后 SIGINT 关闭
超过 10 秒并使用了 SIGTERM。服务端日志尾部为空，因此本文不作缺少证据的
根因断言。

### 13.3 历史基准数据

仓库中 2026-06-03 的报告包含 JSON/Protobuf 历史数据，例如 Protobuf c=10
的 P50 为 1.32 ms、QPS 为 925.8。但这些报告没有记录提交哈希和完整环境，
因此只能作为历史背景，不能作为本次 Phase 0 当前基线。

## 14. 回归检查

- [x] 基础注册和登录
- [x] 基础本地单聊
- [ ] 群聊
- [ ] 离线路径
- [x] JSON 兼容性（小规模冒烟路径）
- [ ] Protobuf 端到端兼容性
- [ ] Redis 跨节点路由

未勾选路径没有实际执行，不得描述为已验证。

## 15. 已知限制

- 逻辑用户状态与原始 TCP 连接强引用映射混合在一起。
- 重复登录策略依赖数据库 `online` 状态，该状态可能过期。
- 协议检测每帧重复执行，并且没有最大帧保护。
- Protobuf 请求桥接只明确支持登录、注册和单聊 payload。
- Redis 转发到本地连接时始终使用 JSON 分帧，即使连接 context 可能是
  Protobuf。
- Reactor 回调仍执行同步密码哈希、MySQL 和 Redis 操作。
- DB 任务、未完成入站帧和出站缓冲区均没有明确的应用层容量限制。
- Redis 观察线程使用 detach，没有明确生命周期。
- SIGINT handler 中调用了复杂应用和数据库逻辑，实测关闭超过 10 秒。
- 注册阶段超时，因此当前没有有效的 c=10 性能基线。
- 文档包含互相冲突的历史性能结论，且顶层架构/API 文档为空。

## 16. 后续工作——有意延后

- 独立定位并测试 c=10 注册超时。
- 定义安全、确定性的关闭生命周期。
- Phase 1：将逻辑 Session 状态与 `TcpConnection` 生命周期分离。
- Phase 2：建立协议边界、固定协议选择、显式解码状态和最大帧限制。
- 更后阶段：消息 ID、ACK/重试、有界队列、慢消费者策略、指标和显式
  分布式 Presence。

## 17. 审查说明

- 当前基线只有在最终 Phase 0 提交包含两文件修复时才可构建。
- c=1 延迟仅有 4 个预热后样本，只能视为冒烟测量，不能宣传为容量结果。
- c=10 尝试明确记录为失败，没有用历史数据或猜测值替代。
- 启动提示期望计划和模板位于 `docs/`，但实际位于被忽略的本地
  `codex.md/`。因此新增可跟踪的 `docs/PHASE_TASKS.md` 供后续阶段使用。

## 18. 提交

Phase 0 主提交：

```text
8ec0b95b5a0ee82e990b24f7e0df2df2988cf4e5
chore(evolution): freeze chat server baseline
```

本次中文化属于文档修正，不改变 Phase 0 的实现、测试数据或技术结论。
