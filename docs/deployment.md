# Deployment Guide

## Native Development

已验证 Ubuntu 24.04.4。安装 README 所列依赖及 Muduo，然后：

```bash
cp conf/chatserver.example.json conf/chatserver.local.json
# 修改数据库密码等本地参数
export CHAT_DB_ADMIN_PASSWORD='admin-password'
python3 script/init_db.py --config conf/chatserver.local.json
./script/build.sh
./script/run_server.sh conf/chatserver.local.json
```

初始化脚本不会把 admin password 放入 argv 或日志；fresh schema 与 Phase 7 upgrade 均可重复执行。Redis 无预初始化步骤。

## Native Two-node Cluster

示例 A/B 使用相同 MySQL/Redis，监听 6000/6001，node ID 分别为 `chat-server-a/b`：

```bash
./script/run_cluster.sh
python3 script/demo_two_node.py
./script/stop_cluster.sh
```

生产环境应复制配置并替换密码、node ID、监听地址和基础设施地址，不应直接复用示例凭据。

## Docker Compose

```bash
cp .env.example .env
# 修改 .env
docker compose build
docker compose up -d
docker compose ps
python3 script/demo_two_node.py
docker compose logs chatserver-a chatserver-b
docker compose down -v
```

Compose 服务：

| Service | Host port | Purpose |
|---|---:|---|
| mysql | 3307 | shared durable state/history |
| redis | 6380 | Presence and Streams |
| chatserver-a | 6000 | node `chat-server-a` |
| chatserver-b | 6001 | node `chat-server-b` |

MySQL 与 Redis healthcheck 成功后节点才启动。MySQL fresh volume 自动按 `sql/` 初始化最终 schema。`depends_on` 只用于本地 Compose；跨主机部署仍需外部健康检查、restart policy 和 secret management。

## Configuration Overrides

容器支持：`CHAT_SERVER_IP/PORT/NAME`、`CHAT_NODE_ID`、`CHAT_MYSQL_HOST/PORT/USER/PASSWORD/DATABASE`、`CHAT_REDIS_HOST/PORT`。其余 flow 参数来自 JSON，默认见 `conf/chatserver.example.json`。

## Clients

```bash
./bin/ChatClient 127.0.0.1 6000
./bin/ChatClient 127.0.0.1 6000 --proto
QT_ROOT=/path/to/Qt ./qt_client/build.sh
./qt_client/build/bin/ChatClient
```

Qt GUI 不在 Docker Compose 中，当前连接地址固定为 `127.0.0.1:6000`。

## Operational Notes

- 每个实例必须使用稳定且唯一的 `node_id`。
- MySQL/Redis 数据目录需要持久卷及正式备份策略。
- 不要将 `.env` 或真实配置提交到 Git。
- 优雅关闭用 SIGINT/SIGTERM；kill -9 会依赖 lease、Stream PEL 和 durable pending 恢复。
