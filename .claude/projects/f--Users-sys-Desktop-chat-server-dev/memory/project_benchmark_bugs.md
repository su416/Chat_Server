---
name: project_benchmark_bugs
description: 4 benchmark script bugs discovered and fixed — root cause analysis
type: project
---

压测脚本修复完成（任务 #1 的配套工作）。

**已修复的 4 个 Bug：**

1. **bench_ts 不回传** — 服务端不认识的 JSON 字段被丢弃，接收端永远收不到时间戳
   - 修复：改用在同一进程内用 time.perf_counter() 打 send/recv 时间戳，按索引匹配算 e2e 延迟

2. **recv_one_json 多消息粘包** — TCP 一次 recv 可能含多条 \0 分隔 JSON，旧代码 json.loads 只取第一条
   - 修复：改用 split(b"\0",1) 按分隔符切割，剩余数据缓存到 sock._bench_buf

3. **接收者超时过早** — 固定 10s join 超时不够
   - 修复：自适应 grace_timeout = max(5, messages*0.1)

4. **send_json 耗时被当作 e2e 延迟** — 离线场景测的是内核缓冲区写入速度
   - 修复：移除了无用的 bench_ts 字段，明确指标含义

**How to apply:** 脚本已修复，后续优化（连接池等）后重新跑压测即可获得正确的 e2e 延迟对比数据。
