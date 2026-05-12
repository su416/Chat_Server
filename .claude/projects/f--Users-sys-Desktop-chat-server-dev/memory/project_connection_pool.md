---
name: project_connection_pool
description: MySQL connection pool implementation in progress — current step and remaining file list
type: project
---

MySQL 连接池实现进度（任务 #2 of 10 项优化计划）。

**已完成：**
- [x] 新建 `include/server/db/connectionpool.h` — 头文件：单例 + init() + getConnection()
- [x] 新建 `src/server/db/connectionpool.cpp` — 实现：shared_ptr 自定义 deleter 归还连接

**待完成（按顺序）：**
- [ ] 修改 `include/server/db/db.h` — 新增 `ping()`、参数化 `connect(host,port,user,pwd,db)`
- [ ] 修改 `src/server/db/db.cpp` — 实现 ping，connect 改用 Config 凭据
- [ ] 修改 `src/server/main.cpp` — 启动时初始化连接池
- [ ] 修改 CMakeLists.txt — 添加 connectionpool.cpp
- [ ] 修改 5 个 model 文件 — MySQL mysql → auto mysql = pool.getConnection()

**Why:** 10 项性能优化计划的任务 #2。压测发现 P50=2907ms 端到端延迟，根因是每次 DB 操作都 TCP 握手/挥手。

**设计关键点：**
- 同步阻塞池，shared_ptr + 自定义 deleter 实现 RAII 自动归还
- 池大小 = 4（= Muduo 线程数）
- 归还时 mysql_ping() 健康检查，断连自动重建
- 凭据从 Config 类读取（当前 db.cpp 硬编码）

**How to apply:** 每轮对话只修改一对 .h/.cpp 文件。按上述待完成列表顺序推进。
