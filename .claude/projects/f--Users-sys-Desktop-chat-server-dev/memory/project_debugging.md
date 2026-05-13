---
name: project_debugging
description: segfault 定位 + Valgrind 泄漏修复过程记录
type: project
---

稳定性排查两步完成：

**Segfault (redis.cpp:152)：**
- 工具链：dmesg → addr2line → GDB
- 崩溃地址 0x10（空指针+偏移），error 4（读未映射内存）
- 根因：`reply->element[2]` 访问前未检查 `reply->elements >= 3` 和 `reply->type == REDIS_REPLY_ARRAY`
- 修复：增加类型检查 + 元素数检查 + element[1] 和 element[2] 的 null 检查
- 复现条件：高并发下 Redis Pub/Sub 返回异常格式回复

**Valgrind 内存泄漏 (connectionpool.cpp:26)：**
- definitely lost: 32 bytes + indirectly lost: 3,076,056 bytes → 全部来自 ConnectionPool::init 的 `new MySQL()` 未 delete
- 修复：添加 `~ConnectionPool()` 析构和 `shutdown()` 方法，释放池中所有连接
- 修复后：definitely lost = 0, indirectly lost = 0
- Invalid read/write: 0 条

**How to apply:** 后续如果遇到运行时崩溃，先查 dmesg 有无 segfault，用 addr2line 定位，再 GDB 复现。
