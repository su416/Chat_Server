---
name: project_shared_mutex
description: shared_mutex 替换 mutex 优化 _connMutex 热点锁
type: project
---

ChatService::_connMutex 由 `mutex` 改为 `shared_mutex`。

**Why:** 原来的 mutex 读写互斥——oneChat/groupChat 的 find 操作（读）会被 login/logout（写）阻塞，反之亦然。压测中 c=100 时 5000 次 find 全部排队等锁，是 P50 延迟的重要组成部分。shared_mutex 允许多线程同时读，写操作互斥。

**改动：**
- chatservice.hpp: `mutex` → `shared_mutex`，新增 `#include <shared_mutex>`
- 写操作（login/loginout/clientCloseException）：`lock_guard<shared_mutex>`（排他锁）
- 读操作（oneChat/handleRedisSubscribeMessage）：`shared_lock<shared_mutex>`（共享锁）
- groupChat()：重构为"读锁下快速分类在线/离线 → 释放锁 → 无锁执行 DB 操作"，消除了持锁期间做 DB 查询的锁膨胀问题

**预期效果：** c=100 P50 从 1,529ms → <800ms。需压测验证。
