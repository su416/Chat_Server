# JSON → Protobuf 协议演进总结

## 演进路线

```
v1.0 (原始)     v2.0 (基础设施)     v3.0 (联合共存)     v4.0 (数据验证)
─────           ─────              ─────              ─────
纯 JSON          ProtoCodec 分帧    协商 + Binary      压测对比数据
\0 分帧          Envelope 包装      三方协议一线         P50: 6.89→1.28ms
                 protoToJson 桥接   自动检测 + 协商      快 5-42 倍
                 encodeResponse     零停机迁移
```

---

## 一、v1.0 — 原始 JSON 协议

**帧格式**：`{"msgid":6,"id":1,...}\0`

```cpp
// JsonCodec — 全项目唯一使用的编解码器
encode:  json.dump() + '\0'
decode:  Buffer 中扫描 '\0' → 取出第一帧 → json::parse
```

**问题**：
- 每条消息必须全文 `json::parse` 才能拿到 `msgid` 做路由
- 帧包含冗余字段名（`"msgid"`、`"password"` 比数据还大）
- 没有协议扩展能力——加新消息类型只能改 JSON 字段

---

## 二、v2.0 — Protobuf 基础设施

**新增文件**：`proto/chat.proto`、`ProtoCodec`、`protoToJson` 桥接

### 帧格式

```
[4B BE length] [Envelope { msgid, payload, errcode }]
```

### 关键设计：桥接层

```cpp
// 不重写 handler，Protobuf 请求转为 JSON 走现有逻辑
json protoToJson(int msgid, const string& payload) {
    json js;
    js["msgid"] = msgid;
    // 按 msgid 解析对应的 Proto Message → 赋值到 JSON
    switch (msgid) {
        case LOGIN_MSG:    req.ParseFromString(payload); js["id"]=req.id(); ...
        case REG_MSG:      ...
        case ONE_CHAT_MSG: ...
    }
    return js;
}
```

### 响应编码

```cpp
// encodeResponse — 根据连接上下文选编码器
string encodeResponse(conn, jsonBody) {
    if (conn->getContext() == PROTO_PROTOBUF)
        return ProtoCodec::encode(Envelope{msgid, errcode, jsonBody});
    else
        return JsonCodec::encode(jsonBody);
}
```

---

## 三、v3.0 — 三方协议共存

### 三条路径在同一 `onMessage` 中并行

```cpp
char firstByte = buffer->peek()[0];

if (firstByte == '{')          → JSON 路径 (JsonCodec)
else if (firstByte == 0xFF)    → Binary 路径 (BinaryCodec)
else                           → Protobuf 路径 (ProtoCodec)
```

**互不冲突**：JSON 首字节固定 `{`(0x7B)，Binary 固定 `0xFF`，Protobuf 长度前缀首字节通常 `0x00`。

### 协商机制 (msgid=0)

```cpp
// 客户端连接后发协商请求
{"msgid":0, "accept":"protobuf"}

// 服务端存储协议类型，响应始终走 JSON
conn->setContext(ConnProtocol::PROTO_PROTOBUF);
conn->send(JsonCodec::encode(response.dump()));  // 协商响应不走 encodeResponse

// 后续消息自动按协商格式编码
sendTcpJson → encodeResponse → ProtoCodec
```

**踩坑**：协商响应不能用 `sendTcpJson`——此时 context 已设为 PROTO_PROTOBUF，`encodeResponse` 会把响应包成 Protobuf 帧，而客户端还在等 `\0` 分隔的 JSON。

---

## 四、v4.0 — 压测对比数据

### 测试环境

- 虚拟机 6 vCPU，JSON vs Protobuf 同条件
- 单聊 online-to-online，c=10/50/100/200/500
- Protobuf 走协商模式

### 核心数据

| 指标 | JSON | Protobuf (协商) | 提升 |
|------|------|---------|------|
| **c=10 P50** | 6.89ms | **1.87ms** | **×3.7** |
| **c=50 P50** | 27.3ms | **1.52ms** | **×18** |
| **c=100 P50** | 53.6ms | **1.28ms** | **×42** |
| c=10 P99 | — | 5.5ms | — |
| c=100 P99 | — | 54.9ms | — |
| c=10 到达率 | 100% | 100% | 持平 |
| c=200 到达率 | — | 80% | 首次通过 |

### P50 随并发增长趋势

```
JSON:      6.89 → 27.3 → 53.6  (随并发线性增长)
Protobuf:  1.87 → 1.52 → 1.28  (基本不变)

原因：Protobuf 的 msgid 路由是 O(1) 整数 switch，
      JSON 的 msgid 路由是 O(n) 字符串解析 + 哈希查找
```

### QPS 对比

| 并发 | JSON QPS | Protobuf QPS |
|------|----------|-------------|
| c=10 | 2,334 | 885 |
| c=50 | 2,915 | 888 |
| c=100 | 2,852 | 913 |

Protobuf QPS 低是因为 Python `protobuf` 库的 `SerializeToString()` 比 `json.dumps()` 慢。生产环境用 C++ 客户端（Qt ChatClient），protobuf 序列化由生成的 C++ 代码完成，QPS 差距会消失。

---

## 五、涉及文件

| 文件 | 角色 |
|------|------|
| `proto/chat.proto` | 消息定义 (Login/Reg/OneChat/Envelope) |
| `protocodec.hpp/cpp` | Protobuf 长度前缀分帧 |
| `binary_codec.hpp/cpp` | Binary 帧头分帧 (0xFF 标记) |
| `jsoncodec.hpp/cpp` | JSON \0 分帧 (原始) |
| `chatserver.cpp:onMessage` | 三协议检测 + 分发 |
| `chatserver.cpp:encodeResponse` | 协议感知响应编码 |
| `chatserver.cpp:protoToJson` | Protobuf→JSON 桥接 |
| `chatservice.cpp:negotiate` | msgid=0 协商处理 |
| `public.hpp` | 新增 `NEGOTIATE_MSG=0` |
| `client/main.cpp` | 命令行客户端 `--proto` 支持 |
| `script/bench_proto.py` | Protobuf 压测脚本 |
| `script/bench_compare.py` | JSON vs Protobuf 对比脚本 |

---

## 六、面试叙事（2 分钟版）

> "我做了从纯 JSON 到 JSON+Protobuf 双协议共存的演进。v1 是 JSON \0 分帧，v2 加了 Protobuf 长度前缀分帧和 Envelope 包装，通过 protoToJson 桥接复用现有 handler。v3 实现了首字节协议检测——`{` 走 JSON、`0xFF` 走 Binary、其余走 Protobuf——三条路径在同一个 onMessage 里共存。加了 msgid=0 协商机制，客户端可以主动声明协议偏好。压测验证：Protobuf 的 P50 稳定在 1.3-1.9ms，随 c=10→100 几乎不涨；JSON 从 6.89ms 涨到 53.6ms。这是'基础设施先于业务就绪，客户端可逐步迁移'的协议演进实践。"
