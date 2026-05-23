# ChatServer 项目复盘计划

## 目标

逐模块吃透整个项目的架构、源码、设计决策和性能数据，能做到面试中任何一个点都被追问 2-3 层而不卡壳。

## 时间安排

共 7 天，每天 1-2 个模块，平均 1-2 小时/天。

---

## 第一天：整体架构 + 编译运行

### 目标
从零启动项目，理解顶层结构。

### 阅读清单

1. `README.md` — 项目全貌
2. `CMakeLists.txt` — 根构建文件
3. `src/server/CMakeLists.txt` — 服务端编译依赖
4. `conf/chatserver.json` — 配置文件

### 动手

```bash
./script/build.sh && ./bin/ChatServer
python3 script/bench_onechat.py  # 只看 c=10 梯度
```

### 重点理解

- Muduo 的 Reactor 模型：主线程 accept → Round-Robin 分发 → 工作线程处理
- 整个请求链路：TCP 包到达 → onMessage → JsonCodec 分帧 → json::parse → getHandler → login/oneChat/groupChat
- CMake 如何组织 src/server/ 下的子目录

### 自检问题

1. 客户端发 `{"msgid":6,"id":1,"toid":2,...}\0` 之后，服务端经历了哪些处理步骤？
2. `thread_num=4` 改了会发生什么？改成 1 会怎样？
3. `aux_source_directory` 做了什么？

---

## 第二天：数据模型层 + 数据库

### 目标
吃透所有 Model 类和数据库交互模式。

### 阅读清单

1. `include/server/model/user.hpp` + `usermodel.cpp` — 用户 CRUD
2. `include/server/model/group.hpp` + `groupmodel.cpp` — 群组逻辑
3. `include/server/model/offlinemessagemodel.cpp` — 离线消息
4. `include/server/model/chathistorymodel.cpp` — 聊天历史
5. `sql/chat.sql` + `sql/chat_history.sql` — 表结构
6. `include/server/model/friendmodel.cpp` — 好友关系

### 重点理解

- 每张表的设计意图（user 为什么有 state 字段？offlinemessage 为什么不用外键？）
- Prepared statement vs sprintf 拼接 SQL 的区别
- `mysql_use_result` vs `mysql_store_result`
- `GroupModel::queryGroups` 为什么用了两次查询（先查群列表，再查群成员）

### 自检问题

1. `login()` 中有哪几个 DB 操作？顺序有要求吗？
2. `groupChat()` 的离线消息扇出是怎么做的？当前实现有什么问题？我们怎么优化的？
3. `chatmessage` 表的 `groupid` 字段为什么可以是 NULL？

---

## 第三天：连接池 + 异步 DB 线程池

### 目标
吃透数据库连接管理的设计演进。

### 阅读清单

1. `include/server/db/db.h` + `db.cpp` — MySQL 封装
2. `include/server/db/connectionpool.h` + `connectionpool.cpp` — 连接池
3. `include/server/utils/db_thread_pool.hpp` + `db_thread_pool.cpp` — 异步线程池

### 重点理解

- 优化前：每次 DB 操作 `new MySQL → connect → query → close → delete`
- 优化后：启动时预建 4 个连接 → getConnection 借 → shared_ptr 析构自动还
- 归还时的健康检查：`mysql_ping()` → 断了的自动重建
- 为什么用 `shared_ptr` 而非 `unique_ptr`？（类型擦除 deleter）
- 异步线程池：Muduo IO 线程 submitVoid → DB 线程执行 → IO 线程不阻塞
- `_cv.wait()` 和 `_cv.wait_for()` 的区别

### 自检问题

1. `getConnection()` 和 `getConnection(int timeoutMs)` 的实现区别？什么时候用哪个？
2. `wrapConnection()` 的 deleter 里，连接断开后是怎么重建的？重建失败怎么办？
3. `DBThreadPool::submitVoid` 传的 lambda 捕获了什么？生命周期安全吗？
4. 如果 4 个连接全被借出，第 5 个线程调 `getConnection()` 会怎样？

---

## 第四天：ChatService 业务逻辑

### 目标
吃透所有业务 handler 的执行流程。

### 阅读清单

1. `include/server/chatservice.hpp` + `chatservice.cpp` — 全部 handler
2. `include/server/chatserver.hpp` + `chatserver.cpp` — TCP 服务层
3. `src/server/jsoncodec.cpp` — JSON 分帧
4. `src/server/main.cpp` — 启动流程

### 重点理解

- `login()` 的完整链路：查用户 → 状态检查 → 加连接表 → Redis 订阅 → 更新状态 → 查离线消息 → 查好友 → 查群组 → 拼响应
- `oneChat()` 的转发判定：本地在线 → DB 查状态 → Redis publish → 离线存储
- `groupChat()` 的扇出：查群成员 → 读锁分类在线/离线 → 在线转发 → 离线查状态 → 存离线
- `handleRedisSubscribeMessage()`：跨服消息的接收处理
- `shared_mutex` vs `mutex` 的使用位置和原因

### 自检问题

1. `login()` 中为什么先加锁 `_userConnMap.insert`，再 `_redis.subscribe`，再 `_userModel.updateState`？顺序能换吗？
2. `oneChat()` 为什么不先查 `_userConnMap` 就直接落库历史消息？
3. `clientCloseException()` 为什么是遍历 `_userConnMap` 找连接，而不是用 userid 索引？
4. 说出 3 处用到 `_connMutex` 的地方，分别是什么锁类型？

---

## 第五天：Redis 跨服转发

### 目标
吃透 Redis Pub/Sub 的使用方式和容错设计。

### 阅读清单

1. `include/server/redis/redis.hpp` + `redis.cpp` — Redis 类完整实现
2. 回顾 `chatservice.cpp` 中所有 `_redis.xxx()` 调用

### 重点理解

- 双上下文：publish_context + subscribe_context，为什么需要两个连接？
- observer 线程：`redisGetReply` 阻塞接收 → 解析三元素数组 → 回调 ChatService
- 断线重连：内层 while 收消息 → 退出 → 外层 while 3s 重试 → `reconnect()` 重建双上下文
- 降级逻辑：publish 失败 → `isConnected()` 返回 false → 存入离线消息表
- `redisAppendCommand` + `redisBufferWrite` 的异步模型（subscribe 不阻塞）

### 自检问题

1. Redis Pub/Sub 的消息格式是什么？`reply->element[0/1/2]` 分别是什么？
2. 我们修过的 segfault 在 `redis.cpp:152`，具体是什么问题？怎么修的？
3. 如果 Redis 宕机，正在发送的消息会怎样？Redis 恢复后呢？
4. `subscribe()` 为什么用 `redisAppendCommand` 而不是 `redisCommand`？

---

## 第六天：Qt 客户端 + 协议层

### 目标
吃透客户端架构和双协议设计。

### 阅读清单

1. `qt_client/` 下的 Controller → Network → UI 三层
2. `proto/chat.proto` — Protobuf 消息定义
3. `src/server/protocodec.cpp` — Protobuf 分帧
4. `src/server/chatserver.cpp` onMessage — 协议检测

### 重点理解

- MVC 分层：Controller 管信号槽，View 管 UI，Model 管数据
- TCP 粘包：`onReadyRead` → `m_recvBuf.append` → `indexOf('\0')` 逐帧解析
- QAbstractListModel：`beginInsertRows/endInsertRows` → QListView 自动刷新
- 双协议检测：`buffer->peek()[0] == '{'` → JSON，否则 → Protobuf
- 桥接层：`protoToJson(msgid, payload)` → 复用现有 handler
- Envelope 包装：所有 PB 消息统一用 `{msgid, payload}` 路由

### 自检问题

1. 客户端 `ChatClient` 怎么处理 TCP 收到的半帧数据？
2. `FriendListModel::addFriend()` 为什么先调 `beginInsertRows` 再 `append` 再 `endInsertRows`？
3. 如果客户端发 Protobuf，服务端怎么知道用哪个 handler？
4. `encodeResponse()` 怎么知道该用 JSON 还是 Protobuf 编码？

---

## 第七天：性能 + 工程化 + 面试模拟

### 目标
能独立讲出完整的优化故事，每题追问能答两层。

### 复习清单

1. `docs/bench_results/` — 查看历次压测 JSON
2. `docs/chat_server_architecture_and_benchmark.md` — 性能对比表
3. `docs/performance_optimization_journey.md` — 优化历程
4. `Dockerfile` + `docker-compose.yml` — 部署方案

### 四轮优化一句话总结

```
连接池        → 消除 TCP 握手，P50 -93%
shared_mutex  → 多线程并发读，读锁不互斥
批量查询      → N+1 → 1 次 SQL，群聊 DB 减少 N 倍
异步 DB 池    → IO 线程零阻塞，P50 再 -83%，并发 200 突破
```

### 面试模拟

用 2 分钟讲完整个优化故事：

> "我主导了一次完整的性能优化闭环。首先自研压测脚本建立基线，发现 localhost P50 高达 2984ms。通过代码审查定位到每次 DB 操作都创建销毁连接，设计了 shared_ptr + 自定义 deleter 的连接池，P50 降到 217ms。然后做了热点锁优化和群聊 N+1 批量查询。最后实现异步 DB 线程池，将写操作从 Muduo IO 线程剥离，P50 再降 83% 到 37ms。调试中还用 dmesg+GDB 修复了 Redis 数组越界的 segfault，Valgrind 验证了零泄漏。整个优化让 P50 从 2984ms 降到 37ms，降幅 98.8%。"

### 可能被追问的点及应答

| 追问 | 应答方向 |
|------|----------|
| 连接池为什么用 shared_ptr 不是 unique_ptr？ | 类型擦除 deleter，unique_ptr 需要显式指定 deleter 类型 |
| shared_mutex 比 mutex 快吗？ | 低竞争时更慢（原子计数器开销），高并发读时快很多 |
| 异步 DB 线程池消息可靠性怎么保证？ | 当前 fire-and-forget，允许极端崩溃时丢失——适合聊天场景，支付需要 WAL |
| 200 并发 109ms 能继续优化吗？ | 换 Protobuf 协议、少一次 JSON 解析、上 DPDK 旁路内核 |
| 为什么不用连接池框架（如 BoneCP）？ | C++ 没有标准池，自研更可控、面试更亮 |
| QPS 为什么只在 6000-8000，不算高？ | TCP 协议栈 + JSON + 虚拟机开销，去掉这些裸测可达 5 万+ |

---

## 每天检查清单

完成当天模块后，确认能做到：

- [ ] 能画出该模块的类图/数据流图
- [ ] 能说出该模块的关键设计决策和原因
- [ ] 能回答自检问题中的至少 3 题
- [ ] 能定位到该模块的关键源码行数
