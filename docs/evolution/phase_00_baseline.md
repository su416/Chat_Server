# Phase 00 — Freeze and Measure the Existing Baseline

- Branch: `evolution/realtime-gateway`
- Start commit: `c00212d2df267c72eb0cd49c3846585d1e717832`
- End commit: pending
- Date: 2026-09-02 (Asia/Hong_Kong)
- Build type: not explicitly set (`CMAKE_BUILD_TYPE` empty; root CMake adds `-g`)
- Verification environment: Ubuntu 24.04.4 LTS virtual machine, 6 vCPU, 7.7 GiB RAM

## 1. Background

The repository is a C++17 clustered chat server built on Muduo. It provides JSON,
binary, and partial Protobuf access; login and presence state; direct and group
chat; offline/history persistence in MySQL; Redis Pub/Sub cross-node forwarding;
a Qt client; and Python benchmark scripts.

Phase 0 freezes and documents that implementation. It does not introduce gateway
abstractions, sessions, ACKs, retries, or backpressure.

## 2. Observed Problem

The repository contained useful performance and protocol documents, but there was
no reproducible record tied to the current commit. Inspection also found that the
current commit did not build before a minimal repair:

- `PasswordUtils::hashPassword` and `verifyPassword` declarations had been
  accidentally removed from `password_utils.hpp`;
- a stray `8` in `password_utils.cpp` caused a syntax error;
- the earlier VM result (5/5 tests passing) was from `15ff1c5`, not the current
  commit `c00212d`, so it was rejected as current baseline evidence.

The two password files were restored byte-for-byte to their known-good state at
`15ff1c5`. No password behavior or algorithm was changed.

## 3. Baseline

### Git state

The local repository began on `dev` at `c00212d`, three commits ahead of the
Gitee-tracking `origin/dev`. The GitHub-tracking `github/dev` also pointed to
`c00212d`. The working tree already contained the requested `.gitignore` change.
A dedicated `evolution/realtime-gateway` branch was created without rewriting
history.

The Linux VM was freshly cloned from GitHub at the same commit. Its working tree
contained only the two-file minimal build repair during verification.

### Exact build and test commands

```bash
git restore \
  --source=15ff1c5ce1d73941660f31ffdfcff95c654e3485 \
  -- include/server/utils/password_utils.hpp \
     src/server/utils/password_utils.cpp

./script/build.sh
ctest --test-dir build --output-on-failure
```

`script/build.sh` removes and recreates `build/`, generates Protobuf C++ sources,
runs CMake, and builds with `make -j$(nproc)`.

### Build result

```text
[100%] Linking CXX executable /home/ys/Desktop/chat-server-phase0/bin/ChatServer
[100%] Built target ChatServer
-rwxrwxr-x ... ./bin/ChatServer
```

Result: PASS after the documented minimal repair.

### Automated test result

```text
1/5 test_rate_limiter ......... Passed
2/5 test_jsoncodec ............ Passed
3/5 test_protocodec ........... Passed
4/5 test_password_utils ....... Passed
5/5 test_friendlistmodel ...... Passed

100% tests passed, 0 tests failed out of 5
Total Test time (real) = 0.56 sec
```

### Source-tree overview

| Area | Role | Files observed |
|---|---|---:|
| `include/` | public and server headers | 23 |
| `src/` | server and CLI client implementation | 22 |
| `tests/` | CTest/GoogleTest tests | 6 |
| `script/` | build, generation, benchmark, and perf scripts | 10 |
| `proto/` | Protobuf schema | 1 |
| `qt_client/` | Qt desktop client | 30 |

The tracked `docs/api.md` and `docs/architecture.md` files are empty. Useful
architecture evidence currently lives in README and the historical architecture,
performance, test, and protocol-evolution documents.

## 4. Hypothesis

Freezing the real implementation, including its current failure modes and
measurement limitations, will allow later phases to make one justified change at
a time and compare against evidence instead of README claims.

## 5. Alternatives Considered

1. Use the successful test result from `15ff1c5`: rejected because it was not the
   current source revision.
2. Revert the repository to `15ff1c5`: rejected because it would discard later
   history and violate the no-history-rewrite goal.
3. Repair only the accidental password declaration/syntax damage: selected; the
   result exactly matches the known-good versions of those two files.
4. Fix the benchmark timeout and shutdown issue in Phase 0: rejected; those are
   baseline findings for a later scoped change.

## 6. Decision

Preserve `c00212d` as the start revision, apply only the two-file build repair,
record both the failed and successful verification, and stop architectural work
after producing this baseline.

No ADR was created because Phase 0 made no architectural decision.

## 7. Architecture Before

```text
JSON / Binary / Protobuf clients
              |
              v
Muduo TcpServer (main loop + 4 worker loops)
              |
       ChatServer::onMessage
  first-byte protocol detection + framing
              |
              v
ChatService singleton + handler map
  |-- user_id -> shared TcpConnection map
  |-- synchronous model reads/updates
  |-- unbounded asynchronous DB write queue
  |-- Redis Pub/Sub forwarding
  |-- token-bucket rate limiter
              |
      +-------+--------+
      v                v
 MySQL pool       Redis Pub/Sub
```

## 8. Architecture After

Unchanged. Phase 0 adds documentation and restores buildability only.

## 9. Implementation

### Files added

- `docs/evolution/phase_00_baseline.md`
- `docs/PHASE_TASKS.md`

### Files modified

- `.gitignore`: ignore the root-local `codex.md/` directory.
- `include/server/utils/password_utils.hpp`: restore two existing declarations.
- `src/server/utils/password_utils.cpp`: remove the stray `8`.

### Important implementation details

- The password repair matches commit `15ff1c5`; it is not a new implementation.
- No SessionManager, internal envelope, ACK, retry, queue bound, or metrics feature
  was introduced.

## 10. Concurrency and Lifetime Analysis

### Networking and connection state

- Muduo owns the `TcpServer` and its connection callbacks.
- Configuration selects four worker threads (`one loop per thread`).
- `ChatService` is a process-lifetime singleton.
- `_userConnMap` stores strong `TcpConnectionPtr` values keyed by user ID and is
  protected by `shared_mutex`.
- Login, logout, and disconnect mutate the map under an exclusive lock. Routing
  reads use a shared lock.
- Some calls release the map lock before network/DB work, but Redis subscription
  recovery iterates the map under a shared lock while issuing Redis operations.

### Protocol state

- New connections default to JSON.
- `onMessage` guesses protocol from the first byte for every frame and overwrites
  connection context; negotiated state is therefore not authoritative.
- JSON frames end at NUL. Protobuf frames use a 4-byte big-endian length. Binary
  frames use `0xFF`, one-byte message ID, and a 2-byte length.
- Partial and multiple frames are supported by the codecs.
- No codec enforces a maximum frame size. JSON can buffer indefinitely without a
  NUL, and a Protobuf length can request an unbounded buffer wait.

### Database work

- The MySQL connection pool has four connections and blocks indefinitely when the
  pool is empty on the default acquisition path.
- Direct/group history writes are submitted to a separate four-thread DB pool.
- The DB task queue is an unbounded `std::queue`; submission has no result and no
  saturation policy.
- `DBThreadPool::shutdown()` sets `_running=false`. Because `workerLoop()` uses
  `while (_running)`, workers can exit with queued tasks still pending; shutdown
  does not guarantee a full drain.

### Redis lifetime

- Redis uses synchronous hiredis contexts.
- The subscription observer is a detached thread capturing the `Redis` object by
  reference. There is no explicit stop/join lifecycle.
- Reconnect loops indefinitely with three-second sleeps.
- Pub/Sub is non-durable and cannot support a durable-delivery claim.

## 11. Failure Modes

| Failure | Current observed/expected behavior | Verified? |
|---|---|---|
| Current revision before repair | Compile errors in password utility | Yes |
| Partial JSON/Protobuf frame | Retained until complete | Unit-tested |
| Multiple JSON/Protobuf frames | Extracted one at a time | Unit-tested |
| Oversized/missing frame delimiter | No configured bound | Code inspection |
| MySQL pool exhausted | Reactor or DB worker may wait indefinitely | Code inspection |
| DB queue overload | Queue grows without a bound | Code inspection |
| Redis publish failure | Direct chat falls back to offline DB storage | Code inspection |
| Redis subscriber disconnect | Detached observer retries every 3 seconds | Code inspection |
| c=10 benchmark registration | Registration response timed out | Yes |
| SIGINT shutdown after smoke | Did not exit within 10 seconds; TERM used | Yes |

## 12. Tests

### Automated tests

The repaired current revision passed all five registered CTest targets in 0.56 s.
There are no server integration tests registered with CTest.

### Smoke test

Command:

```python
from script.bench_onechat import run_online_to_online
run_online_to_online(1, 5)
```

Result:

```text
scenario: online_to_online
concurrency: 1
total messages: 5
success: 5
failed: 0
receiver errors: 0
sender errors: 0
elapsed: 0.073 s
QPS: 68.4
measured latency count after warm-up: 4
P50: 0.54 ms
P95: 0.57 ms
P99: 0.57 ms
```

This verifies registration, login, local online routing, JSON NUL framing, and a
small asynchronous history-write path. It does not verify group, offline,
cross-node Redis, or Protobuf end-to-end behavior.

## 13. Benchmark / Fault Injection

### Environment

- CPU: 6 virtual CPUs, host model reported as 13th Gen Intel Core i9-13900HX
- RAM: 7.7 GiB; 3.8 GiB swap
- OS: Ubuntu 24.04.4 LTS; Linux 7.0.0-28-generic x86_64
- Compiler: GCC 13.3.0
- CMake: 3.28.3
- Protobuf: 3.21.12
- MySQL client/server package version: 8.0.46
- Redis server: 7.0.15
- Python: 3.12.3
- Build: `CMAKE_BUILD_TYPE` not explicitly configured; `-g` added globally
- Commit: `c00212d` plus the documented two-file build repair
- Date: 2026-09-02T01:38:20+08:00 for environment capture

### Representative attempt

Parameters:

- scenario: online-to-online JSON
- concurrency: 10 pairs
- messages per pair: 50
- planned requests: 500
- warm-up: first 10% per pair excluded from latency statistics

Result: INVALID / no performance result. Registration timed out before the load
phase while creating `bench1c_1788287539_c10_oto_ur1`:

```text
RuntimeError: register failed for ..._ur1: None
```

No P50/P95/P99, throughput, or delivery success rate is reported for this attempt
because the workload never began. The failure is itself baseline evidence. After
the preceding successful smoke run, SIGINT shutdown also exceeded ten seconds and
required SIGTERM. Server log tail output was empty, so no unsupported root-cause
claim is made here.

### Historical benchmark artifacts

Tracked reports dated 2026-06-03 contain JSON/Protobuf figures, including a
Protobuf c=10 P50 of 1.32 ms and 925.8 QPS. They do not record a commit hash or the
complete required environment and are therefore historical context only, not the
Phase 0 current baseline.

## 14. Regression Check

- [x] basic registration/login
- [x] basic local single chat
- [ ] group chat
- [ ] offline path
- [x] JSON compatibility (small smoke path)
- [ ] Protobuf end-to-end compatibility
- [ ] Redis cross-node route

Unchecked paths were not run and must not be described as verified.

## 15. Known Limitations

- Logical user state is conflated with a strong raw TCP connection map.
- Duplicate-login policy depends on database `online` state and can become stale.
- Protocol detection is repeated per frame and has no maximum-frame protection.
- Protobuf request bridging only explicitly maps login, registration, and direct
  chat payloads.
- Redis-delivered payloads are always sent with JSON framing, even when the local
  connection context may be Protobuf.
- Reactor callbacks still perform synchronous password hashing, MySQL operations,
  and Redis operations.
- DB tasks, inbound incomplete frames, and outbound buffers have no explicit
  application-level bounds.
- Redis observer lifetime uses a detached thread.
- SIGINT handling calls complex application/DB code from a signal handler and the
  observed shutdown exceeded ten seconds.
- No current valid c=10 performance baseline exists because setup timed out.
- Documentation contains conflicting historical performance claims and two empty
  top-level architecture/API documents.

## 16. Follow-up — Deferred on Purpose

- Determine and test the c=10 registration timeout independently.
- Define a safe, deterministic shutdown lifecycle.
- Phase 1: separate logical Session state from `TcpConnection` lifetime.
- Phase 2: establish a protocol boundary, sticky protocol selection, decode status,
  and maximum frame size.
- Later phases only: message identity, ACK/retry, bounded queues, slow-consumer
  policy, metrics, and explicit distributed presence.

## 17. Review Notes

- The current baseline is buildable only with the two-file repair included in the
  eventual Phase 0 commit.
- The c=1 timing is a smoke measurement with only four post-warm-up samples; it
  must not be marketed as a capacity result.
- The c=10 attempt is intentionally recorded as failed rather than replaced with
  historical or guessed numbers.
- The start prompt expected plan/template files under `docs/`, but they were found
  under the local ignored `codex.md/` directory. A tracked phase checklist is added
  under `docs/` for continuity.

## 18. Commit

Proposed message:

```text
chore(evolution): freeze chat server baseline
```

Actual commit:

```text
The commit containing this file; resolve with:
git log -1 --format=%H -- docs/evolution/phase_00_baseline.md
```

The literal hash is not embedded because a commit cannot contain its own final
hash without changing that hash. The completion report records the resolved value.
