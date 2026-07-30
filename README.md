# ChatServer — 组织内部即时通讯平台

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.10%2B-green)](https://cmake.org/)
[![Qt6](https://img.shields.io/badge/Qt-6.11-green)](https://www.qt.io/)
[![MySQL](https://img.shields.io/badge/MySQL-8.0-orange)](https://www.mysql.com/)
[![Redis](https://img.shields.io/badge/Redis-7-red)](https://redis.io/)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-24.04-purple)](https://ubuntu.com/)

基于 C++17 + Muduo + MySQL + Redis 的组织内部即时通讯平台，配套 Qt6 图形客户端与自动化压测工具链。

## 系统架构

```
Qt6 图形客户端 / 命令行客户端
        │
        ▼  TCP :6000  (JSON / Protobuf 双协议)
┌───────────────────────────────────────┐
│           ChatServer (Muduo)           │
│  主 Reactor → 4 工作 Reactor 线程      │
│       ↓ onMessage                     │
│  ChatService (单例, shared_mutex)      │
│   ├── ConnectionPool  (MySQL 长连接)   │
│   ├── DBThreadPool    (异步 DB 写入)   │
│   ├── RateLimiter     (令牌桶限流)     │
│   └── Redis Pub/Sub   (跨服转发+重连)  │
└───────┬──────────────────┬────────────┘
        ▼                  ▼
   ┌──────────┐     ┌──────────────┐
   │ MySQL 8  │     │  Redis 7     │
   │ 持久化    │     │  Pub/Sub     │
   └──────────┘     └──────────────┘
```

## 性能优化总览

| 优化阶段 | c=10 P50 | c=50 | c=100 | c=200 |
|----------|----------|------|-------|-------|
| 优化前（无连接池） | 2984ms | 注册失败 | 注册失败 | 注册失败 |
| + MySQL 连接池 | **217ms** (-93%) | 通过 | 通过 | 注册失败 |
| + shared_mutex + 批量查询 | 330ms | 通过 | 通过 | 注册失败 |
| + **异步 DB 线程池** | **37ms** (-83%) | **43ms** | **60ms** | **109ms** |
| **累计提升** | **-98.8%** | **突破** | **突破** | **突破** |

| 指标 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| c=10 P50 | 2984ms | 37ms | **×80** |
| QPS | 75 | 8,195 | **×109** |
| 群聊 mixed 5/20/50 | 全部失败 | 全部通过 | — |

## 技术栈

| 层次 | 技术 | 说明 |
|------|------|------|
| 网络 | Muduo | 多 Reactor + one loop per thread |
| 数据库 | MySQL 8.x | 连接池 + Prepared Statement |
| 消息 | Redis 7.x | Pub/Sub 跨服转发 + 自动重连 |
| 序列化 | JSON + Protobuf | 双协议共存 |
| 安全 | SHA-256 (OpenSSL) | 密码哈希 + 盐值 |
| 构建 | CMake 3.10+ | — |
| 客户端 | Qt6 Widgets | MVC 架构 + Model/View |
| 测试 | Python 压测脚本 | 多并发梯度自动执行 |

## 功能特性

- 注册登录、状态管理、好友添加、群组创建与加入
- 单聊 + 群聊在线实时转发，离线消息存储与推送
- 聊天历史记录查询
- 跨服务器消息转发（Redis Pub/Sub + 断线自动重连）
- MySQL 连接池（RAII 归还 + 健康检查 + 自动重建）
- 异步 DB 线程池（写操作不阻塞 IO 线程）
- 令牌桶限流 + Redis 宕机降级 + MySQL 超时保护
- JSON + Protobuf 双协议共存（首字节检测 + 桥接）
- 密码 SHA-256 哈希 + Valgrind 零泄漏验证

## 快速开始

### 依赖

```bash
sudo apt install -y build-essential cmake g++ \
  libmysqlclient-dev libhiredis-dev libprotobuf-dev protobuf-compiler \
  nlohmann-json3-dev libssl-dev
```

### 构建服务端

```bash
./script/build.sh
```

### 初始化数据库

```bash
sudo mysql -u root -p < sql/chat.sql
sudo mysql -u root -p < sql/chat_history.sql
sudo mysql -u root -p -e "ALTER TABLE chat.user MODIFY password varchar(64) NOT NULL;"
```

### 启动

```bash
sudo systemctl start redis-server
./bin/ChatServer
# 观察：ConnectionPool initialized → connect redis-server success!
```

### 构建 Qt 客户端

```bash
cd qt_client
./build.sh
./build/QtChatClient
```

### 运行压测

```bash
python3 script/bench_onechat.py       # 单聊 (10→500 并发)
python3 script/bench_groupchat.py     # 群聊 (5→50 人群)
# 报告 → docs/bench_results/
```

## 目录结构

```
├── conf/                  # 服务端配置
├── include/server/        # 头文件
│   ├── db/               # 连接池
│   ├── model/             # User/Friend/Group/Offline/ChatHistory
│   ├── redis/             # Redis Pub/Sub
│   └── utils/             # 密码哈希 / 限流器 / DB 线程池
├── src/server/            # 服务端源码
├── proto/                 # Protobuf 定义
├── qt_client/             # Qt6 客户端（独立 CMake 项目）
├── script/                # 构建 + 压测脚本
├── sql/                   # 数据库初始化
├── docs/                  # 压测报告 + 架构文档
└── Dockerfile / docker-compose.yml
```

## 优化技术清单

| # | 优化项 | 技术 | 效果 |
|---|--------|------|------|
| 1 | 连接池 | shared_ptr + 自定义 deleter | TCP 握手瓶颈消除 |
| 2 | 热点锁 | shared_mutex 读写分离 | 多线程并发读 |
| 3 | 批量查询 | WHERE id IN (...) | N+1 → 1 次查询 |
| 4 | 异步 DB | 独立线程池 + fire-and-forget | IO 线程零阻塞 |
| 5 | 内存安全 | Valgrind Memcheck | 零泄漏验证 |
| 6 | 崩溃排查 | dmesg + addr2line + GDB | segfault 精确定位 |
| 7 | 双协议 | Protobuf + JSON 桥接 | 零停机迁移 |
| 8 | 故障降级 | 超时 + Redis 降级 + 限流 | 优雅容错 |
| 9 | 密码安全 | SHA-256 + 盐值 (OpenSSL) | 非明文存储 |
| 10 | 工程化 | Docker Compose + 配置化 | 一键部署 |

## 核心数据

> P50 延迟 2984ms→37ms 降 98.8%，QPS 75→8195 升 109 倍，并发 200 零丢失。用 dmesg+addr2line+GDB 定位 segfault，Valgrind 验证零泄漏。shared_mutex 读写锁 + 异步 DB 线程池 + 群聊 N+1 批量查询。Protobuf 双协议共存 + 令牌桶限流 + Redis 降级自动重连。

## License

MIT
