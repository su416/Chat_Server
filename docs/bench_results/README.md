# 压测基线数据

## 使用方法

```bash
# 单聊压测（5 个并发梯度，每个执行在线+离线双场景）
cd ChatServer
python3 script/bench_onechat.py

# 群聊压测（3 个群规模，每个执行全部离线+混合在线双场景）
python3 script/bench_groupchat.py
```

## 输出文件

每次运行生成 `docs/bench_results/bench_<run_id>.json`，包含：

- `run_id`, `timestamp`, `server`
- 每个并发梯度 / 群规模的 `qps`, `elapsed_seconds`, `latency` (avg/p50/p95/p99/p999)

## 可调参数

在脚本顶部修改变量：
- `CONCURRENCY_LEVELS` / `GROUP_SIZES`
- `MESSAGES_PER_PAIR` / `MESSAGES_PER_GROUP`
- `WARMUP_RATIO`

## 优化前后数据对比

每次优化后重跑同参数，对比两份 JSON 报告即可得到量化数据。
