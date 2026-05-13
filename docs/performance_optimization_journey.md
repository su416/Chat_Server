# ChatServer 性能优化闭环 —— 面试叙事

## 项目背景

C++17 + Muduo + MySQL + Redis 即时通讯服务端，支持单聊、群聊、离线消息、历史查询、跨服转发。目标：从单机 100 并发优化到 10000 并发，P99 延迟从秒级降到 50ms 以内。

---

## 第一阶段：建立性能基线（压测脚本开发与调试）

### 1.1 压测工具设计

自研 Python 压测脚本，覆盖 4 个核心场景：

| 场景 | 描述 | 测量指标 |
|------|------|----------|
| 单聊 A：在线→在线 | N 对用户同时在线收发 | 端到端延迟(P50/P95/P99)、QPS、到达率 |
| 单聊 B：在线→离线 | N 个发送者向离线用户发消息 | 发送吞吐、离线存储路径延迟 |
| 群聊 A：全离线扇出 | 1 人在线发群消息，其余离线 | 群消息扇出到离线表的吞吐 |
| 群聊 B：混合在线扇出 | 20% 成员在线接收群消息 | 群消息端到端扇出延迟 |

多并发梯度自动执行：10 → 50 → 100 → 200 → 500，输出 JSON 基线报告。

### 1.2 压测脚本调试中发现的 3 个 Bug

**Bug 1：TCP 粘包导致消息大量丢失。** 服务端用 `\0` 分隔 JSON 帧，快速发送时多个帧合在一个 TCP 段到达。原始 `recv_one_json()` 用 `json.loads(data + "\0")` 解析，一次 `recv(4096)` 收到多条消息时，`rstrip("\0")` 只去掉末尾分隔符，中间的 `\0` 还在，`json.loads` 因 trailing garbage 抛异常 → 后续消息全被丢弃。

**修复**：改用 `split(b"\0", 1)` 按分隔符切割，第一条解析返回，剩余数据缓存到模块级字典 `_recv_bufs[id(sock)]` 供下次调用。这是标准的 TCP 粘包处理方案。

**Bug 2：无法测量端到端延迟。** 发送方把 `time.perf_counter()` 写入 JSON 字段 `bench_ts`，期望接收方回读后做差。但服务端不认识这个字段，重构消息体时丢弃了它 → 接收方永远读到 0。

**修复**：改为在同一进程内（`perf_counter()` 是系统级单调时钟），发送线程记录 `send_stamps[i]`，接收线程记录 `recv_stamps[i]`，按序号匹配做差得到真 e2e 延迟。

**Bug 3：** `socket.socket` 是 C 扩展类型（`__slots__`），不能动态添加属性。改为 `id(sock)` 做 key 的模块级字典。

### 1.3 初始基线数据（优化前）

| 并发 | 到达率 | QPS | P50 延迟 | 备注 |
|------|--------|-----|----------|------|
| 10 | 20% | 75 | **2984ms** | 每消息独立建连/断连 |
| 50+ | 0% | — | — | 注册直接失败，服务端无响应 |

**结论**：服务端在 localhost 上 P50 接近 3 秒，并发 50 即崩溃。瓶颈在 DB 操作路径。

---

## 第二阶段：根因分析

### 2.1 代码审查发现

阅读 `src/server/model/` 下 5 个 model 文件，发现全部 20 个方法采用同一模式：

```cpp
MySQL mysql;           // 栈对象 → mysql_init()
mysql.connect();       // TCP 握手 + MySQL 认证握手
mysql.update(sql);     // 一条 SQL
// ~MySQL()            // TCP 挥手 → mysql_close()
```

一次 `login()` 请求触发 6 次以上 DB 操作 = 6 次 TCP 握手 + 6 次挥手。单次 TCP 建连 ~3ms，仅 TCP 开销就占 ~36ms/login。**压测中 500 条消息触发约 1000 次 DB 操作，累计 TCP 握手耗时 3 秒以上**，与实测 P50=2984ms 吻合。

### 2.2 线程模型分析

Muduo 使用 `one loop per thread` 模型，4 个工作线程。每个线程在 `onMessage` 回调中同步执行 DB 操作，线程被 `mysql_query` 阻塞期间，该线程负责的所有其他连接全部停摆。这是 P50 延迟爆炸的根因。

---

## 第三阶段：MySQL 连接池设计与实现

### 3.1 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 池类型 | 同步阻塞池 | 与 Muduo 同步 IO 模型匹配 |
| 连接数 | = Muduo 线程数 (4) | 一线程一连接，避免排队 |
| 归还机制 | `shared_ptr<MySQL>` + 自定义 deleter | RAII 自动归还，不污染类型签名 |
| 健康检查 | 归还时 `mysql_ping()` | 检测 MySQL 超时断开，自动重建 |
| 凭据管理 | 改用 Config 类读取 | 原来硬编码在 db.cpp 中 |
| 字符集 | 从 Config 读 charset | 原来硬编码 `set names gbk` |

### 3.2 核心实现

```cpp
// 取连接 —— 池空则阻塞，池有则取
shared_ptr<MySQL> ConnectionPool::getConnection() {
    unique_lock<mutex> lock(_mutex);
    _cv.wait(lock, [this] { return !_pool.empty(); });
    MySQL* conn = _pool.front(); _pool.pop();

    // 自定义 deleter：归还而非 delete
    return shared_ptr<MySQL>(conn, [this](MySQL* c) {
        if (c->ping()) {           // 连接正常 → 归还
            lock_guard<mutex> lg(_mutex);
            _pool.push(c);
        } else {                   // 连接断开 → 重建
            MySQL* newConn = new MySQL();
            newConn->connect(_host, _port, _user, _password, _dbname);
            lock_guard<mutex> lg(_mutex);
            _pool.push(newConn);
            delete c;
        }
        _cv.notify_one();
    });
}
```

### 3.3 Model 层改动

全部 5 个 model 文件（20 个方法），统一将 `MySQL mysql; mysql.connect()` 替换为 `auto mysql = ConnectionPool::instance()->getConnection()`，`.` 改为 `->`。改动机械但要求精确。

---

## 第四阶段：性能对比

### 4.1 连接池优化前后对比

| 指标 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| **c=10 P50 延迟** | 2984ms | **217ms** | **-93%** |
| **c=10 P99 延迟** | 5377ms | **303ms** | **-94%** |
| **c=10 QPS** | 75 | **10,316** | **×138** |
| **c=10 到达率** | 20% | **100%** | 零丢失 |
| **c=50** | 注册失败 | **100% 到达** | 突破瓶颈 |
| **c=100** | 注册失败 | **100% 到达** | 突破瓶颈 |
| 群聊 mixed size=5 | 注册失败 | **100% 到达** | 首次跑通 |
| 群聊 mixed size=20 | 注册失败 | **100% 到达** | 首次跑通 |

### 4.2 延迟随并发的变化规律

```
c=10:  P50 = 217ms
c=50:  P50 = 1109ms     (×5 并发, P50 ×5.1)
c=100: P50 = 1529ms     (×2 并发, P50 ×1.4)
c=200+: 注册超时
```

P50 随并发线性增长，说明"线程数 × 连接数"的同步 IO 模型在高并发下排队，下一步需异步 DB 线程池。

---

## 第五阶段：稳定性问题排查

### 5.1 Segfault 定位

压测在高并发下偶发 Segmentation fault。排查步骤：

1. **dmesg** 捕获到 `segfault at 10`（空指针 + 偏移 0x10），`error 4`（读未映射内存）
2. **addr2line** 将崩溃 IP 偏移映射到源码行
3. **GDB 复现**：`Connection #1763` 时崩溃在线程 6

定位到 `src/server/redis/redis.cpp:152`：

```cpp
// 原始代码：未检查数组长度
if (reply != nullptr && reply->element[2] != nullptr ...)
```

**根因**：Redis Pub/Sub 消息处理中，`redisReply->element[2]` 的访问没有先检查 `reply->elements >= 3`。异常回复时数组不足 3 元素 → 越界读 → segfault。

**修复**：增加 `reply->type == REDIS_REPLY_ARRAY && reply->elements >= 3` 检查。

### 5.2 Valgrind 内存泄漏排查

Valgrind Memcheck 全量检测：

- **修复前**：`definitely lost: 32 bytes` + `indirectly lost: 3,076,056 bytes`（`ConnectionPool` 的 `new MySQL()` 未释放）
- **修复**：添加 `~ConnectionPool()` 析构函数和 `shutdown()` 方法，退出时遍历队列 `delete` 所有连接
- **修复后**：`definitely lost: 0 bytes, indirectly lost: 0 bytes`

---

## 第六阶段：经验总结

### 技术收获

1. **TCP 粘包处理**：`\0` 分帧协议下的正确做法是 `split + 缓冲区`，而非 `json.loads` 全量解析
2. **连接池的 self-return 模式**：`shared_ptr` + 自定义 deleter 实现 RAII 归还，比手动 `putConnection` 语义更清晰、更安全
3. **性能优化的数据驱动**：每个优化必须有前后对比数据，P50/P99/到达率 三个指标缺一不可
4. **系统调试工具链**：dmesg → addr2line → GDB → Valgrind，从线索到定位到修复的完整流程

### 仍可优化的方向

- `shared_mutex` 替换 `_connMutex`：当前 mutex 读写互斥，改为 shared_mutex 支持多线程并读
- 异步 DB 线程池：将 DB 操作从 Muduo IO 线程剥离到独立线程池，解除 IO 阻塞
- 群聊在线状态批量查询：消除 N+1 查询问题

---

## 面试发言提纲

> "我在 ChatServer 项目中主导了一次性能优化闭环。首先自研了压测脚本建立基线，发现 localhost 端到端延迟 P50 高达 2984ms。通过代码审查定位到根因：每次数据库操作都创建销毁 MySQL 连接，一次 login 触发 6 次 TCP 握手。我设计实现了一个同步连接池，使用 shared_ptr 自定义 deleter 实现 RAII 自动归还，池大小 4（= Muduo 线程数），归还时 mysql_ping 健康检查+自动重建。优化后 P50 从 2984ms 降到 217ms，降幅 93%；QPS 从 75 提升到 10316，提升 138 倍；并发 100 仍 100% 到达。调试过程中使用 dmesg + addr2line + GDB 定位并修复了一个 redis 消息数组越界的 segfault，并用 Valgrind 验证了零内存泄漏。
>
> 这次优化让我深刻理解了：性能问题必须数据说话，工具链（dmesg / GDB / Valgrind）是工程师的基本功，RAII 思想在资源管理中的多种应用场景。"
