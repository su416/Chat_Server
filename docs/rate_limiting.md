# Simple Rate Limiting & Backpressure

普通聊天采用两层本节点 Token Bucket，加上目标 Stream 长度准入。只在 `ChatService::oneChat/groupChat` 完成认证、Session/owner 校验后调用 limiter；ACK、协商、Presence heartbeat、Stream consumer 的 XACK/XAUTOCLAIM、Pending retry 不消耗聊天配额。

```text
ONE_CHAT / GROUP_CHAT
       ↓
per-user bucket
       ↓
gateway global bucket
       ↓
local delivery / offline persist / remote route
                                  ↓
                          Stream high watermark
                                  ↓
                          Stream hard limit
                                  ↓
                                XADD
```

## 配置与兼容

```json
"rate_limit": {
  "enabled": false,
  "user_capacity": 1000,
  "user_rate_per_sec": 10000.0,
  "global_capacity": 5000,
  "global_rate_per_sec": 3000.0
},
"flow": {
  "stream_soft_limit": 8000
}
```

这些是保留旧行为的演示默认值，未作为生产容量建议进行调优。`enabled=false` 同时关闭 user/global quota；Stream 背压独立生效。旧 `capacity/rate_per_sec` 继续可用，新 `user_*` 字段优先。省略 rate_limit 时禁用；字段存在但省略 enabled 时也禁用。容量/速率必须为正，非有限或非法值导致启动失败。配置只在启动时加载，不支持在线 reload。

## Token Bucket

使用单调时钟按经过时间补充令牌，最多补至 capacity。每个请求消耗一个 user token 和一个 global token；只有两者都足够时才一起扣减。单 mutex 保护配置、用户 map、节点 bucket 和计数器，保留原有 `allow()` 接口。锁内只有内存运算，无 I/O、sleep 或等待队列空间。群聊按请求收取一个 token，没有 weighted fan-out cost。

## Stream 水位与安全边界

Phase 8 已经存在 `flow.stream_soft_limit`，本次复用为 high watermark，没有新增重复阈值。Phase 5 的硬上限仍为 10000 条。单次 Redis Lua 原子完成 XLEN 检查和 XADD，避免多个源节点同时检查后突破水位。

| XLEN | 结果 |
|---|---|
| 小于 high watermark | 允许 XADD |
| 达到 high watermark 且小于 10000 | HighWatermark，拒绝新的 route |
| 达到或超过 10000 | HardLimit，拒绝新的 route |

先检查 hard，再检查 soft，因此异常/外部 producer 导致的硬上限可以单独观测。恢复消费并经原有安全裁剪降到水位以下后自动恢复准入。XLEN 是保守的积压代理，含 PEL、未消费数据以及尚未裁剪的已确认数据，不等于单独的 PEL 数。没有删除未确认 payload，也未修改 safe trim、dedup、DLQ、owner migration 或 XAUTOCLAIM。

## 客户端错误与指标

沿用 `ONE_CHAT_MSG_ACK=18` / `GROUP_CHAT_MSG_ACK=19` 的错误结构，新增 `reason` 和 `retryable`：

| reason | errno | 语义 |
|---|---:|---|
| RATE_LIMITED | 3 | 用户桶耗尽，尚未接受 |
| SERVER_OVERLOADED | 4 | 节点桶耗尽，尚未接受 |
| TARGET_OVERLOADED | 4 | 目标 Stream 拒绝，尚未 XADD |

单聊拒绝可稍后重试。群聊已部分扇出后遇到 Stream 背压，返回 `partial=true,retryable=false`，不能自动重发整组造成重复；群聊仍不是 durable fan-out。已有 transport/infrastructure 错误保持原语义。Redis 响应丢失时，原有“XADD 可能成功但源节点未获知”的不确定性依旧存在。

周期 `CLUSTER_METRICS` 增加 `rate_limit_user_rejected`、`rate_limit_global_rejected`、`route_backpressure_rejected`、`stream_hard_rejected`。`stream_overload_rejected` 作为原指标继续保留；与 `route_backpressure_rejected` 是同一计数，不应相加。分类拒绝日志首条及每 100 次记录一次。

## 验证

```bash
./script/build.sh
ctest --test-dir build --output-on-failure
./script/gen_python_proto.sh
python3 script/traffic_governance_integration.py
python3 script/phase6_phase3_regression.py
python3 script/phase4_two_node_integration.py
python3 script/phase5_stream_integration.py
```

新增 C++ 测试覆盖独立用户、确定性 refill、精确并发全局容量、拒绝不误扣另一层令牌、配置兼容与非法参数；Redis 组件测试覆盖 high watermark 拒绝/恢复、PEL 不被驱逐、硬上限及多 producer 原子准入。真实 socket 测试覆盖单聊/群聊两级拒绝、配额耗尽时 durable ACK 完成、heartbeat 继续运行、Stream 拒绝后无新 durable row，以及恢复后所有已接纳消息 ACKED。

## 边界

global 指一个 Gateway 进程，不是分布式总配额。仍使用单 mutex，没有引入 sharding 或新调度器。user bucket map 沿用原有 cleanup 接口，未新增独立内存治理。没有引入新的阻塞 I/O；继承的 Presence lookup、Stream producer 等同步 Redis 路径和部分 legacy MySQL 路径仍可在 I/O loop 上执行，本轮没有声称全链路异步。没有测试过生产多机容量或证明 limiter mutex 是性能热点。
