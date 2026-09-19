# Architecture

## 1. Overview

Chat_Server 是一个 C++17/Muduo 多节点实时消息网关。Redis 维护最终一致的 Presence 和可恢复的跨节点 Stream；MySQL 保存业务数据、聊天历史以及客户端投递状态；每个 ChatServer 只管理本节点真实 TCP Session。

```text
                  Redis
       ┌────────────────────────┐
       │ Presence: user -> node │
       │ per-node Redis Streams │
       └───────────┬────────────┘
                   │
      ┌────────────┴────────────┐
      │                         │
┌─────▼───────────┐       ┌─────▼───────────┐
│ ChatServer A    │       │ ChatServer B    │
│ SessionManager │       │ SessionManager │
│ MessageRouter  │       │ MessageRouter  │
│ PendingManager │       │ PendingManager │
└─────┬───────────┘       └─────┬───────────┘
      │ JSON / Protobuf         │ JSON / Protobuf
      ▼                         ▼
   Client A                  Client B
      │                         │
      └──────── MySQL ──────────┘
           history + pending_delivery
```

## 2. Components

- `ProtocolState/MessageDecoder/MessageEncoder`：连接级协议选择和 framing。
- `MessageEnvelope/MessageRouter`：内部消息与业务 handler 边界。
- `SessionManager`：本节点 user/session/connection 映射及 replacement。
- `PresenceManager`：Redis owner lease、heartbeat、lookup、compare-and-delete。
- `RouteTransport`：每节点 Stream、Consumer Group、PEL、XAUTOCLAIM、route dedup/DLQ。
- `DurablePendingModel`：MySQL 客户端投递 source of truth。
- `PendingDeliveryManager`：当前进程、当前 Session 的有界运行时 Pending。
- `DBThreadPool`：High/Normal/Low 有界异步 DB 队列。

## 3. Thread Model

| Execution context | Responsibility |
|---|---|
| Muduo I/O loops | decode、Session 生命周期、连接 context、最终 send |
| DB WorkerPool | durable insert/update、ACK、history、offline claim |
| Redis Stream consumer | XREADGROUP/XAUTOCLAIM、解析 route、把工作排入目标 I/O loop |
| Presence worker | node/user lease refresh、连接数刷新 |
| Main-loop timers | 500 ms delivery/inbox scanner、5 s metrics |

只有连接所属的 Muduo I/O loop 可以读取或修改 `TcpConnection` context。Redis consumer 不直接发送客户端帧；它定位 Session 后 `queueInLoop`，由目标 loop 根据 `ProtocolState` 编码。新 durable 操作不在 I/O loop 等待 DB queue；少数 legacy model 查询仍是同步限制。

## 4. Request Flow

本地单聊：sender → router → local Session → durable pending → client。

跨节点单聊：sender → Presence lookup → source Stream XADD → target Consumer Group → target durable pending → target Session → client。

离线单聊：Presence miss → durable row `WAITING_SESSION`；登录 hook/scanner 按批 claim 为 `PENDING`。

## 5. Reliability Identities

`routeId` 是 Gateway 间一次逻辑路由的身份，用于 Stream 重放去重。`deliveryId` 是目标 Gateway 到客户端的一次 durable delivery 身份，由目标持久层生成，并在 retry/restart/reconnect 后保持不变。二者不能合并。

## 6. Delivery State Machine

```text
WAITING_SESSION ──claim──> PENDING ──durable ACK──> ACKED
       ▲                     │
       └──disconnect/move────┘

WAITING_SESSION / PENDING ──expiry──> EXPIRED
```

`ACKED` 与 `EXPIRED` 是终态。ACK 表示客户端确认帧被服务端持久处理，不表示用户阅读。

## 7. Backpressure

| Resource | Capacity | Full behavior |
|---|---:|---|
| DB task queue | 2048 | normal/low 明确拒绝；ACK high/deferred |
| Runtime Pending | 10000 entries | durable row 保留，停止 claim/activate |
| Runtime Pending bytes | 16 MiB | durable row 保留 |
| Deferred ACK | 10000 IDs | 合并 duplicate；客户端 retry 可再次 ACK |
| Redis Stream soft threshold | 8000 | XADD 前原子 admission reject |
| Socket output high/hard | 4/8 MiB | 暂停恢复；持续慢或过硬限制断开 |

这些值来自默认配置，可在 `flow` 中调整并受启动校验约束。

## 8. Data Model

- `user/friend/allgroup/groupuser`：账户与社交关系。
- `chatmessage`：聊天历史和 legacy offline compatibility，不是可靠投递状态机。
- `pending_delivery`：`route_id` 幂等键、`delivery_id`、owner、payload、retry/expiry 和投递状态。

## 9. Consistency Model

系统采用 At-Least-Once + Idempotency/Dedup + Eventually Consistent Presence。Presence lease 和 owner migration 可能短暂过期；stale route 会校验 owner 并最多重路由一次。Redis Stream 与 MySQL 之间没有分布式事务，基础设施整体丢失不在保证范围内。

## 10. Limitations

群聊没有 durable fan-out；Binary 路径没有完全统一到可靠单聊生命周期；部分 legacy DB/Redis handler 仍同步；没有 Exactly Once、read receipt、多设备同步或跨地域一致性。
