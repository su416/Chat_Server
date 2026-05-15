# ChatServer 架构设计与性能优化报告

## 一、系统架构

```
                         ┌──────────────────────┐
                         │     Qt6 图形客户端     │
                         │   QTcpSocket + JSON   │
                         └──────────┬───────────┘
                                    │ TCP :6000
                                    │ \0 分帧 JSON
                                    ▼
┌───────────────────────────────────────────────────────────────┐
│                        ChatServer                             │
│                                                               │
│  ┌──────────────────────────────────────────────────────┐    │
│  │                  Muduo Reactor 层                      │    │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐ │    │
│  │  │ Main    │  │ Reactor │  │ Reactor │  │ Reactor │ │    │
│  │  │ Reactor │  │ Thread 0│  │ Thread 1│  │ Thread 3│ │    │
│  │  │ accept  │  │ epoll   │  │ epoll   │  │ epoll   │ │    │
│  │  └─────────┘  └────┬────┘  └────┬────┘  └────┬────┘ │    │
│  └────────────────────┼────────────┼────────────┼───────┘    │
│                       │ onMessage  │            │            │
│                       ▼            ▼            ▼            │
│  ┌──────────────────────────────────────────────────────┐    │
│  │                  ChatService (单例)                    │    │
│  │  login │ reg │ oneChat │ groupChat │ loginout        │    │
│  │  shared_mutex ── _userConnMap (在线用户表)             │    │
│  │  RateLimiter ── 令牌桶限流                             │    │
│  └──────┬──────────────────────────────────┬────────────┘    │
│         │                                  │                  │
│         ▼                                  ▼                  │
│  ┌─────────────┐  ┌──────────┐  ┌──────────────────┐        │
│  │ UserModel   │  │  Redis   │  │ ConnectionPool   │        │
│  │ FriendModel │  │ Pub/Sub  │  │ ┌──┐┌──┐┌──┐┌──┐│        │
│  │ GroupModel  │  │ 跨服转发  │  │ │c1││c2││c3││c4││        │
│  │ ChatHistory │  └──────────┘  │ └──┘└──┘└──┘└──┘│        │
│  │ OfflineMsg  │                 └────────┬─────────┘        │
│  └──────┬──────┘                          │                  │
│         │                                 │                  │
└─────────┼─────────────────────────────────┼──────────────────┘
          │                                 │
          ▼                                 ▼
   ┌──────────────┐               ┌──────────────────┐
   │   MySQL 8.x  │               │    Redis 7.x     │
   │  持久化存储   │               │   跨服 Pub/Sub   │
   └──────────────┘               └──────────────────┘

消息流（单聊）：
  Client A ──→ Muduo Reactor ──→ ChatService::oneChat()
    ├── 1. ChatHistoryModel.insertOneChat()  → MySQL (落库)
    ├── 2. _userConnMap.find(toid)           → 本地在线? TCP 转发
    ├── 3. UserModel.query(toid)             → 跨服在线? Redis publish
    └── 4. OfflineMsgModel.insert()          → 离线消息落库
```

## 二、技术栈

| 层次 | 技术 |
|------|------|
| 语言 | C++17 |
| 网络库 | Muduo (多 Reactor + one loop per thread) |
| 数据库 | MySQL 8.x + libmysqlclient |
| 缓存/消息 | Redis 7.x + hiredis Pub/Sub |
| 序列化 | JSON (nlohmann/json) + Protobuf (待启用) |
| 哈希 | SHA-256 (OpenSSL) |
| 客户端 | Qt6 Widgets + QTcpSocket |
| 构建 | CMake 3.10+ |
| 部署 | Docker Compose |

## 三、性能优化闭环

### 3.1 优化路线图

```
基线建立 → 连接池 → 内存安全 → 热点锁 → 批量查询 → 安全加固 → 故障降级
   ↓          ↓        ↓         ↓         ↓         ↓         ↓
 2984ms    217ms    0泄漏    shared_    N+1→1    SHA-256   限流+超时
 P50       P50     Valgrind  mutex    批量查询   密码哈希   优雅降级
  -          -93%     ✅      读写分离    DB优化     ✅        ✅
```

### 3.2 性能对比总表

| 优化阶段 | c=10 P50 | c=10 QPS | c=50 | c=100 | 群聊 mixed 5 | 群聊 all 20 |
|----------|----------|----------|------|-------|-------------|------------|
| 优化前 (无池) | 2984ms | 75 | 注册失败 | 注册失败 | 注册失败 | 注册失败 |
| MySQL 连接池 | **217ms** | **10,316** | **通过** | **通过** | **通过** | **通过** |
| 全优化后 | **330ms** | **6,707** | **通过** | **通过** | **通过** | **通过** |

### 3.3 各优化项详细数据

**第一轮：MySQL 连接池**

| 指标 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| c=10 P50 | 2984ms | 217ms | **-93%** |
| c=10 QPS | 75 | 10,316 | **×138** |
| c=10 到达率 | 20% | 100% | 零丢失 |
| c=50 | 注册失败 | 100% 到达 | 突破 |
| c=100 | 注册失败 | 100% 到达 | 突破 |

**第二轮：稳定性修复**

| 问题 | 工具 | 根因 | 修复 |
|------|------|------|------|
| Segfault (连接#1763) | dmesg + addr2line + GDB | redis.cpp 数组越界 | 增加 elements/type 检查 |
| 内存泄漏 3MB | Valgrind Memcheck | ConnectionPool 未释放 | 析构函数 delete 连接 |
| 压测脚本消息丢失 | 代码审查 | TCP 粘包 + 时间戳不回传 | 缓冲区分割 + 跨线程计时 |

**第三轮：shared_mutex + 批量查询**

| 改动 | 机制 | 效果 |
|------|------|------|
| _connMutex mutex→shared_mutex | 多线程并发读 _userConnMap | 消息转发不再排队等锁 |
| groupChat 锁范围缩小 | 锁内扫码→释放→无锁DB | 锁持有从几十ms降到<1μs |
| queryStates 批量查询 | N 次 SELECT→1 次 WHERE IN | 群聊离线 DB 查询减少 N 倍 |

**第四轮：安全加固 + 故障降级**

| 措施 | 实现 |
|------|------|
| 密码哈希 | SHA-256(password + salt)，OpenSSL |
| MySQL 宕机 | getConnection(timeout) 超时返回 nullptr |
| Redis 宕机 | publish 失败→自动降级离线存储 |
| 单用户限流 | 令牌桶 20/s，配置化开关 |

## 四、踩坑记录

1. **TCP 粘包 + json.loads 不适配 \0 分帧**：一条 recv 含多条 JSON，rstrip 去不掉中间 \0→抛异常→消息丢弃。改为 split("\0") 逐条解析 + 残留缓存。

2. **perf_counter 跨线程计时可靠**：`time.perf_counter()` 是系统级单调时钟，同进程内不同线程可安全做差。

3. **socket.socket 是 __slots__ 类**：不能动态加属性，改用 id(sock) 做 key 的模块级 dict。

4. **全局 mutex 是性能杀手**：限流器加了一把全局锁，P50 涨 124%。修复后加 `_initialized` 检查，关闭时零开销跳过。

5. **低并发时 shared_mutex 不如 mutex**：shared_mutex 用原子计数器，无竞争时比 mutex 多几十个周期。适用场景是读多写少的高并发。

## 五、面试核心数据（一句话版）

> "P50 延迟 2984ms→217ms 降 93%，QPS 75→10316 升 138 倍，并发 100 零丢失。用 dmesg+addr2line+GDB 定位 segfault，Valgrind 验证零泄漏。shared_mutex 并发读 + 批量查询消除 N+1，SHA-256 密码哈希 + 令牌桶限流 + Redis 降级。"

## 六、待优化方向

- 异步 DB 线程池（解除 Muduo IO 线程的 DB 阻塞）
- Protobuf 协议（二进制序列化替代 JSON）
- 分库分表 / 读写分离（规模化方向）
