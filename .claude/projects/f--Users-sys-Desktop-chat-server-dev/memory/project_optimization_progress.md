---
name: project_optimization_progress
description: 10项性能优化计划的最终进度 + 完整性能曲线数据
type: project
---

10 项优化计划全部完成。日期：2026-05-17。

**最终性能数据（四轮优化历程）：**

| 优化阶段 | c=10 P50 | c=50 | c=100 | c=200 | 群聊 mixed 5 | 群聊 mixed 20 |
|----------|----------|------|-------|-------|-------------|--------------|
| 无池 | 2984ms | 失败 | 失败 | 失败 | 失败 | 失败 |
| +连接池 | 217ms | 通过 | 通过 | 失败 | 743ms | 通过(偶然) |
| +shared_mutex+批量查询 | 330ms | 通过 | 通过 | 失败 | 1877ms | 失败 |
| +异步DB线程池 | **37ms** | **43ms** | **60ms** | **109ms(首次!)| **258ms** | **958ms(首次!)** |

**P50 累计降幅：2984ms → 37ms，-98.8%。QPS：75 → 8,195，×109倍。c=200 首次通过，群聊三个梯度全部首次通过。**

**已完成：**
- [x] 任务 #1：压测脚本基线（TCP粘包修复 + e2e延迟测量）
- [x] 任务 #2：MySQL 连接池（shared_ptr deleter 归还 + 健康检查）
- [x] 任务 #3：shared_mutex 替换 mutex（+ groupChat 锁范围重构）
- [x] 任务 #4：群聊批量查询（N+1 → 1次 WHERE IN）
- [x] 任务 #5：Valgrind 零泄漏（ConnectionPool 析构 + redis segfault 修复）
- [x] 任务 #6：SHA-256 密码哈希
- [x] 任务 #7：故障模拟（MySQL超时 + Redis降级 + 令牌桶限流）
- [x] 任务 #8：Docker Compose（Dockerfile 三阶段构建 + mysql/redis/chatserver 编排）
- [x] 任务 #9：Protobuf + JSON 双协议（Envelope + ProtoCodec 分帧 + 桥接 + 响应编码）
- [x] 任务 #10：架构图 + 压测数据报告

**额外任务：**
- [x] Qt 客户端三步改进（TCP粘包+重连 / QAbstractListModel / QSS+QSettings+快捷键）
- [x] Redis 自动重连

**面试核心数据：**
> "四轮优化后 P50 从 2984ms 降到 37ms（-98.8%），QPS 从 75 提升到 8,195（×109），并发 200 零丢失。用 dmesg+addr2line+GDB 定位修复了 redis 数组越界 segfault，Valgrind 验证零内存泄漏。用 shared_mutex 支持多线程并发读，批量查询消除 N+1，异步 DB 线程池将 IO 线程从 DB 阻塞中解放。Protobuf+JSON 双协议通过首字节检测实现零停机迁移。"
