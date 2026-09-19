# Phase 8 Benchmark and Backpressure Results

## Environment

Ubuntu 24.04 VM，Redis/MySQL 同机，debug-style build，单次样本，无内核调优、生产网络延迟或多机隔离。数据用于验证语义与资源边界，不代表最大吞吐。

## Results

| Scale | Protocol | Sent | Accepted | Rejected | Durable ACKed | P50 | P95 | P99 | Unique/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 10×100 | JSON | 1000 | 1000 | 0 | 1000 | 619 ms | 1264 ms | 1308 ms | 449.8 |
| 20×100 | JSON | 2000 | 2000 | 0 | 2000 | 1461 ms | 1701 ms | 1740 ms | 774.9 |
| 50×100 | JSON | 5000 | 4516 | 484 | 4516 | 2993 ms | 4928 ms | 5250 ms | 715.1 |
| 10×100 | Protobuf | 1000 | 1000 | 0 | 1000 | 681 ms | 1231 ms | 1256 ms | 472.8 |

50×100 的 484 条请求在 durable acceptance 前收到明确错误码 4。它们不是 accepted message，也不是静默丢失；这是有界背压的预期行为。

## Resource Pressure

20×100 的 DB queue 达到 2048，发生 165 次 ACK enqueue rejection；deferred ACK 最终回落到 0，2000 个 accepted delivery 全部 durable ACK。50×100 同样保持服务存活并明确拒绝过载请求。慢消费者测试触发一次 high-water 和一次断连，健康客户端延迟 3.72 ms，慢用户重连恢复 80/80。Stream soft limit=8 的故障注入精确峰值为 8，随后 16 条 route 明确拒绝。

## Offline Backlog

| Backlog | Login | First message | All ACKed |
|---:|---:|---:|---:|
| 100 | 49.4 ms | 396.3 ms | 1.84 s |
| 500 | 46.1 ms | 198.3 ms | 7.71 s |
| 1000 | 47.6 ms | 537.1 ms | 16.04 s |

登录不会一次 claim 全量 backlog；claim 继续受到 batch、Pending capacity、DB queue headroom 和 slow-consumer 状态约束。

## Interpretation

Phase 8 优先目标是可靠、有界和明确拒绝，而不是让所有压力规模都被接受。不能把旧版成功子集 latency 与新版完整结果直接比较，也不能把单 VM 的 774.9 QPS 描述为系统上限。
