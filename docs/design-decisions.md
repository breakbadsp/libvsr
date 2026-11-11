# Design decisions for version 1

This file records each decision, the reason, and what is not in version 1.
The code shows how. This file says why.

## Decisions

| Topic | Decision | Reason |
|-------|----------|--------|
| Promise | A generic, reusable protocol library with a documented host contract. Not yet a production claim. | The host contract is the difficult part. A learning implementation does not test it. |
| Language | C++23, GCC and Clang, CMake with Ninja, GoogleTest. | Selected by the owner. Assertions are a library macro (`VSR_ASSERT`) that is always on, because contracts are not available. |
| Failure model | Crash and power loss of any number of replicas. Storage is trusted: a completed sync is durable and correct. | An acknowledged operation must survive a power failure of the full cluster. |
| Durable state | Each replica persists its log, `view`, and `log_view`. It does not use the recovery protocol of the 2012 paper. | The paper's protocol loses all data if all replicas stop together. |
| Log identity | Each entry holds the checksum of the entry before it. | Equal checksums mean equal log prefixes. Replicas compare logs with one number, and recovery ignores old entries without extra state. This is in the format from the start, because a later addition would break existing files. |
| Operations | Opaque bytes. The library never parses them. | Each application has its own commands and format. |
| Replica wire format | Fixed by the library (`include/vsr/message.hpp`). | Checksums and the hash chain depend on the exact layout. |
| Layers | 1: the replica (`vsr`). 2: interfaces for storage, network, state machine, with defaults (`vsr_io`). 3: a runner (`vsr::io::Server`) and a client (`vsr::io::Client`). | A new user starts at layer 3. A host with its own network or disk replaces one part at layer 2. |
| Client path | `Replica::Submit` for a host with its own client protocol. The default client and protocol are optional. | An existing application keeps its gateway. |
| Determinism | The primary puts a timestamp in each operation. Replicas compare checkpoint checksums and halt on a difference. The simulator compares state after each operation. | The library cannot find a clock read in application code. It can supply the agreed input and detect the result. |
| Log size | A ring of fixed size, with checkpoints from the application at fixed operation numbers. A replica that is too far behind gets a snapshot. | The log cannot grow without limit, and equal checkpoint positions make checkpoints comparable. |
| Verification | Deterministic simulation with fault injection, mutation checks of the simulator, a TLA+ model, sanitizers, decoder fuzzing. | See `docs/verification.md`. |

## Decisions made during the build

These differ from the first plan, or were not in it.

- **The replica calls storage directly.** The first plan was "events in, actions out" for everything. The replica calls the `Storage` and `StateMachine` interfaces synchronously and sends messages through `MessageBus`. It stays deterministic, because the host controls all three. The cost: a slow sync blocks the replica thread. `Flush` gives group commit: one sync for each batch of input.
- **Messages do not carry the log.** In the paper, `DoViewChange` and `StartView` carry the log. Here they carry the head of the log (operation and checksum). A replica that enters a view is `Joining`: it finds where its log differs from the log of the primary, removes those entries, and fetches the missing ones. It is `Normal` only when its log is a verified prefix of the primary's log **and** holds every operation up to the head that the primary had when it opened the view (`view_start_op`).

  The second condition is essential. A view change selects the log with the highest `log_view` first. A replica that sets `log_view = v` with a short log could replace a longer log of an older view that holds committed operations. The simulator and the TLA+ model both find this defect when the condition is removed.
- **A restarted primary does not continue.** It starts a view change. A backup can hold an entry that the primary sent but did not sync, and a quorum of backups can commit it. The primary would continue with a shorter log.
- **Truncation is durable by one erased slot.** Later slots stay on storage. The hash chain makes recovery ignore them.
- **Default file storage uses the page cache and `fdatasync`**, not `O_DIRECT`. It is simpler, and the replica reads entries back through the cache.
- **Clients register, and a client without a session cannot execute.** The first design removed the oldest client from a full client table without notice. The review found that a removed client that sends its last request again executes it twice. Now a registration is an operation in the log, a full table removes the session with the oldest commit, and a request without a session gets an `Eviction` message. This follows TigerBeetle. Registrations use operation numbers, so the state machine does not see each number.
- **Quorum is a majority** for replication and for view change. Flexible quorums are not in version 1.

## Not in version 1 (TODO)

Each item names what the current behaviour is.

- **Disk faults** (corrupt reads, lost or misdirected writes). The replica detects a bad log entry or snapshot by checksum and halts. It does not repair from other replicas. Reference: Protocol-Aware Recovery, and the TigerBeetle journal repair.
- **Byzantine faults.** Not planned. The checksum is XXH64, which is not a defence against a malicious peer.
- **Cluster clock synchronization.** The timestamp comes from the clock of the primary, made strictly increasing. A primary with a wrong clock gives wrong timestamps. TigerBeetle rejects such a primary.
- **Reconfiguration** (add, remove, or replace a replica) and **rolling upgrades** with different protocol versions.
- **Read-only requests without the log.** Each request goes through the log, so each read is linearizable and costs one replication round.
- **Pre-vote and primary lease.** A replica that was partitioned starts a view change when it returns. A primary without a quorum does not step down; its clients time out.
- **Asynchronous storage** (`io_uring`) and `O_DIRECT`.
- **Incremental snapshots.** A checkpoint writes the full state, and the replica holds one snapshot buffer in memory.
- **A request queue at the primary.** A request that arrives when the pipeline is full is dropped, and the client sends it again.
- **128-bit checksums**, a C interface, platforms other than Linux for `vsr_io`, IPv6 and host names.
- **`mex` integration.** `mex` adapts to libvsr, in the `mex` repository. See `docs/host-contract.md` for the adapter work.
