# Realtime Gateway Evolution Checklist

Only check an item after it is supported by code inspection or recorded execution
evidence. Do not check future-phase work early.

## Phase 0 — Baseline

### Audit

- [x] Confirm clean/known Git state.
- [x] Record current branch and commit.
- [x] Record source-tree overview.
- [x] Record actual build command.
- [x] Record dependency versions available in the verification environment.
- [x] Record current tests.
- [x] Record current benchmark scripts.
- [x] Record current networking thread model.
- [x] Record current user/connection mapping.
- [x] Record current JSON framing behavior.
- [x] Record current Protobuf framing behavior.
- [x] Record current Redis forwarding path.
- [x] Record current asynchronous DB path.
- [x] Identify blocking calls on Reactor threads.
- [x] Identify unbounded queues, maps, and buffers.

### Verification

- [x] Current source builds after the documented minimal pre-existing regression repair.
- [x] Existing automated tests pass (5/5).
- [x] Basic registration/login/single-message smoke path verified (5/5 messages).
- [x] Representative benchmark attempted; setup timeout documented without invented data.
- [x] Environment recorded.
- [x] `docs/evolution/phase_00_baseline.md` created.
- [x] Proposed baseline commit produced.

## Phase 1 — SessionManager

- [ ] Define Session versus `TcpConnection` responsibility and ownership.
- [ ] Define duplicate-login and stale-connection policy.
- [ ] Add ADR, implementation, invariants, and unit/integration tests.
- [ ] Create `docs/evolution/phase_01_session.md`.

## Phase 2 — Internal Message Envelope / Codec Boundary

- [ ] Define protocol-independent envelope and explicit decode result.
- [ ] Define sticky protocol selection and maximum frame size.
- [ ] Preserve JSON and Protobuf compatibility with tests.
- [ ] Add ADR and `docs/evolution/phase_02_message_envelope.md`.

## Phase 3 — Message ID and ACK State

- [ ] Define and test stable message IDs.
- [ ] Define bounded pending state, TTL, and idempotent ACK semantics.
- [ ] Add ADR and `docs/evolution/phase_03_message_id_ack.md`.

## Phase 4 — Bounded Retry / At-Least-Once

- [ ] Define retry timing, maximum attempts, expiry, and deduplication contract.
- [ ] Test ACK, retry, expiry, late ACK, duplicate ACK, and reconnect behavior.
- [ ] Add ADR and `docs/evolution/phase_04_reliable_delivery.md`.

## Phase 5 — Backpressure

- [ ] Bound the DB queue and handle rejected submissions.
- [ ] Define outbound high-water and slow-consumer policy.
- [ ] Test saturation, recovery, and shutdown under load.
- [ ] Add ADR and `docs/evolution/phase_05_backpressure.md`.

## Phase 6 — Observability

- [ ] Add thread-safe delivery, queue, session, Redis, and latency metrics.
- [ ] Add structured correlation without logging private payloads.
- [ ] Add concurrency tests and `docs/evolution/phase_06_observability.md`.

## Phase 7 — Presence / Explicit Cross-Gateway Routing

- [ ] Define gateway identity, presence schema, TTL, and degraded behavior.
- [ ] Test local, remote, stale, disappearing-node, and Redis restart paths.
- [ ] Add ADR and `docs/evolution/phase_07_presence.md`.

## Phase 8 — Failure Validation / Benchmark / Repositioning

- [ ] Execute the documented Redis, MySQL, ACK, queue, frame, and slow-client faults.
- [ ] Compare basic and reliable paths with complete reproducibility metadata.
- [ ] Update final architecture, reliability, backpressure, failure, and benchmark docs.
- [ ] Create `docs/evolution/phase_08_validation.md`.
