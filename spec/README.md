# libvsr safety model

Run `./spec/run.sh` from the repository root. It downloads TLA+ 1.8.0 into
`spec/.tools/`. It runs every config with `-workers auto` and a 6 GiB heap.
Java and curl are required. `JAVA` can name a Java executable. The script
uses mise if the Java shim has no active version. To select one model, run
`./spec/run.sh Recovery.cfg`. Logs and full mutation traces stay in
`spec/.tools/<config>.log`. A tool error does not count as mutation detection.

The model follows the replica and journal code. It covers durable `view`
and `logView`, written and synced prefixes, loss of any unsynced suffix,
and all three restart cases. Joining and view-change copy use separate
truncate, append, sync, and finish actions. Copy keeps the old `logView`.
It includes quorum view change, synced acknowledgements, commit learning,
and execution. As in `AdvanceCommit`, backups alone can form a quorum;
the primary counts only its synced prefix.

Entries are `(view, value)` records. Message log fields denote exact checksum
identities. They do not transfer logs. Separate actions copy source entries.
Network history uses sets that never remove messages. The highest sent
commit number encodes all earlier commit numbers. This permits loss,
reordering, and duplication. Selection permits every maximal tie.
A crash and restart form one action. A stopped replica can omit input.
Durable view writes are atomic, and completed storage syncs persist.
Initial state follows Format and Recover: replica 0 requests view 1.

The model excludes checkpoints, the log ring, snapshots, client tables,
timestamps, storage corruption, checksum collisions, and liveness.
Guards bound views, log length, and total crashes. Values use symmetry.
These finite checks do not prove the protocol for all bounds.

## Results

Runs used TLC 2026.11.06.222357 from TLA+ 1.8.0, Java 25, and six workers
on 2026-11-11. All models use three replicas. Each row has separate bounds.
No run combines all maximum bounds. Depth and elapsed time come from TLC.
Parallel scheduling can change depth, time, and mutation state counts.

Bounds: values / maximum view / maximum log length / maximum crashes.

| Config | Bounds | Distinct states | Depth | Time | Invariant result | Exhaustive | Mutation length |
|---|---|---:|---:|---|---|---|---:|
| Vsr.cfg | 2/1/1/0 | 805 | 20 | 01s | PASS (all five) | Yes | — |
| Recovery.cfg | 2/2/1/1 | 345,274 | 39 | 58s | PASS (all five) | Yes | — |
| LongLog.cfg | 1/1/3/1 | 1,112,119 | 42 | 03min 57s | PASS (all five) | Yes | — |
| ThirdView.cfg | 2/3/1/0 | 3,984,544 | 48 | 06min 31s | PASS (all five) | Yes | — |
| MutA_JoinNoLength.cfg | 2/2/1/0 | 23,671 | 20 | 13s | FAIL LogViewInvariant | No | 19 |
| MutB_AckUnsynced.cfg | 2/2/1/1 | 8,813 | 14 | 10s | FAIL DurableCommit | No | 12 |
| MutC_PrimaryContinues.cfg | 2/2/1/1 | 83,714 | 19 | 29s | FAIL Agreement | No | 17 |

A baseline PASS covers `TypeOK`, `Agreement`, `DurableCommit`,
`LogViewInvariant`, and `OnePrimaryPerView`. All five passed exhaustively.
Agreement retains every executed entry in a ghost history. DurableCommit
requires a current synced majority. LogViewInvariant retains entries
committed before each durable `logView`. OnePrimaryPerView checks Normal
primary uniqueness. TypeOK checks state domains.
For each mutation, the named invariant failed. Every other invariant result
is incomplete because TLC stops at the first failure. Mutation lengths
count transitions. Each trace has one additional initial state.

Mutation (a) promotes a short prefix after two view changes. Mutation (b)
commits before a synced majority exists. Mutation (c) reuses a primary view
after crash loss, then commits conflicting entries at the same position.
No unmutated run found a protocol defect. A preliminary three-entry run
with two values was stopped. It was not exhaustive. The final LongLog
model uses one value.
