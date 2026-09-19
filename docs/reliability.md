# Reliability Model and Fault Demonstrations

## Two Reliability Layers

### Gateway to Gateway

`routeId` 标识逻辑 route。源节点将内部 `RoutedMessage` 写入目标节点 Stream；Consumer Group 提供 PEL，未 XACK 的条目由 XAUTOCLAIM 恢复；Redis ZSET dedup 和 MySQL `UNIQUE(route_id)` 分别保护 route 执行与 durable delivery 创建。

### Gateway to Client

目标节点在 Stream XACK 之前持久化 `pending_delivery`。客户端帧携带 `deliveryId`；超时、重启和重连继续使用相同 ID。客户端以 ID 去重并回复 ACK；只有 MySQL durable ACK 完成后才从运行时 Pending 清理。

## Semantic Boundaries

- Accepted：durable pending commit 成功，或跨节点层的 Stream durable acceptance；报告必须注明所在层。
- Delivered：客户端收到帧。
- ACKed：客户端 ACK 已持久处理。
- Read：未实现，ACK 不是已读回执。

## Fault Injection

| Case | Expected behavior | Reproduction |
|---|---|---|
| Client drops ACK | same deliveryId retry，client dedup | `python3 script/phase6_phase3_regression.py` |
| target node kill -9 | Stream/PEL/XAUTOCLAIM recovery | `python3 script/phase5_stream_integration.py` |
| persisted before send crash | restart sends same deliveryId | `python3 script/phase6_durable_pending_integration.py` |
| send before ACK crash | restart retry，same ID | 同上 |
| receiver offline | WAITING_SESSION，login 后 claim | `python3 script/phase7_offline_inbox_integration.py --backlog 100 --extended` |
| slow client | watermark/disconnect，durable reconnect recovery | `python3 script/phase8_backpressure_integration.py --case slow` |
| DB saturated | explicit rejection before acceptance | `python3 script/phase8_backpressure_integration.py --case db` |
| Stream saturated | atomic admission rejection | `python3 script/phase8_backpressure_integration.py --case redis` |

## Phase 8 Reliability Regression

早期 20×100 中客户端已发送 2000 个 ACK，但 High DB queue 饱和造成 132 个 ACK 任务丢失，最终部分已接受 delivery 进入 EXPIRED。修复采用按 deliveryId 合并的有界 deferred ACK；queue 恢复后重新提交，deferred 期间 due scanner 不消耗普通 retry/expiry。最终复测在 `ack_enqueue_rejected=165` 时仍达到 `durable_persisted=2000`、`durable_acked=2000`、`pending=0`、`ack_deferred=0`。

## What Is Not Guaranteed

本项目不提供 Exactly Once、分布式事务、基础设施整体丢失保护或 read receipt。Redis/MySQL 的实际 durability 取决于部署配置；客户端去重仍是 At-Least-Once 的必要组成。
