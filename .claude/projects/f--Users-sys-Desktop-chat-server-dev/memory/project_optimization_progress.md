---
name: project_optimization_progress
description: 10项性能优化计划的整体进度追踪
type: project
---

10 项优化计划（CLAUDE.md）整体进度。当前日期：2026-05-13。

**已完成：**
- [x] 任务 #1：改造压测脚本建立性能基线
- [x] 任务 #2：MySQL 连接池
- [x] 任务 #5：Valgrind 内存泄漏排查 + RAII 修复（已完成 ConnectionPool 析构，0 泄漏）
- [x] 任务 #3：shared_mutex 替换 mutex（已完成 chatservice.hpp/cpp）
- [x] 崩溃修复：redis.cpp segfault（越界访问 element[2]）+ 脚本重复注册/群名修复

**当前基线数据：**
- 单聊 c=10: P50=217ms, QPS=10,316, 到达率 100%
- 单聊 c=50: P50=1,109ms, QPS=15,022, 到达率 100%
- 单聊 c=100: P50=1,529ms, QPS=14,939, 到达率 100%
- 群聊 mixed size=5: P50=743ms, 到达率 100%
- 群聊 mixed size=20: P50=2,938ms, 到达率 100%

**待完成：**
- [ ] 任务 #4：群聊在线状态批量查询
- [ ] 任务 #6：密码哈希 bcrypt/SHA-256
- [ ] 任务 #7：故障模拟 + 降级逻辑
- [ ] 任务 #8：Docker Compose 一键部署
- [ ] 任务 #9：Protobuf + JSON 双协议共存
- [ ] 任务 #10：压测数据整理 + 架构图绘制
- [ ] shared_mutex 优化后需重新压测验证效果

**面试叙事数据：**
> "优化前：P50 2984ms，50 并发必挂。优化后：P50 217ms（-93%），100 并发 0 丢失，QPS 从 75 提升到 10,316（×138）。使用 dmesg+addr2line+GDB 定位修复了 redis 数组越界 segfault，Valgrind 验证零内存泄漏。"
