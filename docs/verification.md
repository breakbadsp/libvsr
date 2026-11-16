# Verification

## What runs

| Check | Command | What it shows |
|-------|---------|---------------|
| Unit and integration tests | `ctest --preset gcc` (also `clang`, `gcc-san`, `clang-san`) | The host contract of one replica, the formats, the journal, and a real three-replica cluster on TCP and files. |
| Simulation | `./build/gcc/sim/vsr_sim <first seed> <seed count> <replicas> [chaos ticks] [clients] [--trace]` | Safety with faults, and recovery after the faults stop. |
| Mutation check of the simulator | see below | The simulator fails when the protocol has a known defect. |
| TLA+ model | `spec/run.sh` | The view change and join rules, for all interleavings in a small model. |
| Fuzzing | `-DVSR_BUILD_FUZZ=ON` with Clang, target `vsr_decode_fuzz` | Bytes from the network cannot crash the decoders. |

## The simulator

`sim/simulation.cpp` runs the production replica code with a simulated
network, storage, and clock. One seed gives one run, and the same seed
gives the same run again.

Faults in the first phase: message loss, delay, reordering, and repetition;
replica crashes; power loss during a storage call, where each write that
was not synced is lost, kept, or torn; a partition of one replica; a host
that flushes late; clocks that differ between replicas.

A share of the client requests (`Options::submit_rate_`) goes to
`Replica::Submit`, the path of a host with its own client protocol. The
other requests are `Request` messages for `Replica::OnMessage`. A host
that is not the primary sends a submitted request on to the primary one
time.

Checks during the run:

- Each replica executes operations in order with no gap.
- Each replica executes the same operation at each operation number, and
  has the same state after it.
- Timestamps increase.
- No request executes more than once.
- A client reply names the right operation and has the right body, and the
  operation committed after the request was sent.
- `Submit` gives `NotPrimary` only on a replica that is not the primary,
  `Accepted` only with one new log entry, and `Invalid` only after a
  storage fault.
- Each new application has the same initial state. Each snapshot gives the
  same state in a new application, and a replica that restores a snapshot
  has the state that the first replica had after that operation.
- When an operation first executes, a quorum holds it on durable storage.
- A replica sends `PrepareOk` or `DoViewChange` only when the log and the
  view that the message promises are durable.
- No correct replica counts a protocol violation or an invalid message.

Checks after the faults stop: each client request gets its reply, all
replicas are `Normal` in one view with one primary, and all replicas
executed all operations.

### Run a host application in the simulator

The simulator can run the state machine of a host in place of its own hash
state machine, with the same faults and checks. The interface is in
`sim/include/vsr/sim/simulation.hpp`:

- `vsr::sim::Application` is a `vsr::StateMachine` with one more function,
  `Digest()`: a checksum of the full state. The simulator compares the
  digest and the reply across replicas after each operation.
- `Options::application_factory_` makes one application for each start of
  each replica.
- `Options::body_generator_` makes a valid request body from one random
  number.
- `Options::limits_` sets the replica limits, for example
  `body_bytes_max_` and `snapshot_bytes_max_`.

In CMake, add libvsr with `add_subdirectory(libvsr EXCLUDE_FROM_ALL)` and
link the target `vsr_sim_lib`. Set `VSR_BUILD_TESTS` and
`VSR_BUILD_EXAMPLES` to `OFF` if the project does not need them.
`tests/simulation_test.cpp` has an example, `SumApplication`.

### Find the cause of a failed seed

`vsr_sim <seed> 1 <replicas> --trace` prints one line for each event of
the seed: each fault, each message that is sent, delivered, repeated, or
dropped, each crash and start, each `Submit` result, each completed
request, and each change of the view, the status, or the executed
operation of a replica. Each line starts with the tick. The same seed
gives the same lines. A program that uses the library sets
`Options::trace_`.

## Mutation check

A simulator that never fails proves nothing. `tools/mutation_check.py` puts
one known defect at a time into the code, and the simulator must fail
within 300 seeds. It detects each of these:

- A replica becomes `Normal` without the log of the start of the view.
- A backup acknowledges an entry before the sync.
- The primary counts its own entry before the sync.
- A restarted primary continues as primary.
- Truncation is not durable.
- The view change selects the longest log and ignores `log_view`.
- A replica sends `DoViewChange` before the view is durable.
- A replica learns the commit number while its log is not verified.
- A duplicate of the latest request is prepared and executed again.
- The timestamp is the wall clock without the increase rule.
- A client without a session can execute an operation.

Two defects do not make the simulator fail, because a second barrier in
the code covers each of them:

- A snapshot install that does not erase the slot after the checkpoint.
  Old entries cannot link to the new checkpoint through the hash chain.
- A primary that does not look in the log for a request that is in
  progress. The request is then in the log twice, and the client table
  stops the second execution at commit.

## The TLA+ model

`spec/Vsr.tla` models the parts that differ from the 2012 paper: durable
`view` and `log_view`, the written and synced parts of the log, crashes
that lose the unsynced part, the three restart cases, and the join and
view-change copy as separate truncate, append, and finish steps. TLC checks
agreement, durable commit, the `log_view` invariant, and one primary for
each view. `spec/README.md` has the bounds and the results. The model also
has three mutated configurations, and TLC finds a counterexample for each.

The bounds are small: three replicas, at most three views, and a log of
one to three entries. The model does not prove the protocol for all sizes.

## Known defect

Seed 1780 with three replicas fails with `replica 0 executed a different
operation at op 15`. It is the only failure in seeds 1 to 20000. The cause
is in the journal, not in the simulator:

1. A power loss loses the unsynced entry 14 and keeps the unsynced entry
   15. Recovery stops at entry 13, and the old entry 15 stays in its slot.
2. In the next view the replica gets the same entry 14 again from the new
   primary. The old entry 15 now links to the head of the log.
3. After one more restart, recovery reads the old entry 15 as a part of
   the log. The replica is a backup in an open view, so it does not check
   its log against the primary. It executes the old entry 15, and the
   primary commits a different entry 15.

`TruncateTo` also erases only one slot, and the later entries stay on
storage. No seed failed through that path, but the same sequence applies to
it. A possible correction is to erase each entry after the head, in
recovery and in truncation. The test
`Simulation.DISABLED_StaleEntryAfterLostWrite` runs this seed.

## Known gaps

- The simulator cuts only one replica from the others. It has no
  asymmetric cut and no split into two groups.
- Each replica clock has a fixed offset. The simulator has no drift and no
  clock jump.
- The fault rates are equal for each seed. The simulator does not select a
  different mix of faults for each seed (swarm testing).
- The liveness check runs with all replicas up. No run keeps a minority of
  the replicas down after the faults stop.
- The simulator does not inject disk corruption. That failure type is not
  in version 1.
- The TCP runner and the client have integration tests on one machine.
  They have no fault injection on real networks or disks (Jepsen-style).
- The nightly soak runs for about one hour on four cores. That is hundreds
  of thousands of seeds each night, not millions. A longer soak needs a
  dedicated machine. Run it with the `workflow_dispatch` inputs of the
  `Nightly soak` workflow.

## AlmaLinux CI

PR checks use a prepared AlmaLinux 8 container on the Omarchy `libvsr`
runner. Fork PRs use GitHub-hosted runners. Both GCC and Clang run tests
and simulation seeds for three and five replicas, with and without
ASan/UBSan. The seeds are in two sets. The fixed set is seeds 1..300. It
is the regression set. The fresh set is 200 seeds that start at a value
from the commit SHA, `GITHUB_RUN_ID`, and `GITHUB_RUN_ATTEMPT`. Each run
and each rerun gets new seeds. The log shows each range on a `SEEDS` line.
A failure prints a `REPLAY` line with the command to replay the failed
seed. An assertion abort or a sanitizer stop prints no `FAIL` line, so the
script runs the range again one seed at a time to find the seed. If no
single seed fails, the `REPLAY` line gives the whole range.
`tools/test_sim_seeds.sh` tests this with fake simulators. A failed fresh
seed is a real defect: add it to the tests. A separate job fuzzes the
instrumented decoder for 60 seconds.
Each job keeps its source mount read-only and builds in a disposable
container. Build logs, package versions, and CTest logs are CI artifacts.

The `Nightly soak` workflow runs each day on the default branch, and on
demand with `workflow_dispatch`. It runs on the `libvsr` runner in one
container with at most four CPUs. First it runs `tools/mutation_check.py`
on the writable copy of the source: every known defect must make the
simulator fail. Then four parallel shards run fresh seeds until the time
budget ends (60 minutes by default): two for three replicas, one for five
replicas, and one that takes one, two, and seven replicas in turn. A
shard stops at the first failed seed, and the job fails. The logs
(`soak.log`, `mutation.log`, and `shard-N.log`) are artifacts for 30 days.
The `REPLAY` line in the log gives the command for the failed seed. The
inputs are the first seed, the seed limit for each shard, and the time
budget.

The manual `CI` workflow can also test AlmaLinux 9. The image helper tags
prepared images from the Dockerfile and toolchain script content. To
rebuild a prepared image after a package update, remove that image from
the runner account before the next run.

Omarchy uses one dedicated libvsr runner slot under `gha-runner`. The
triangulator slots keep their existing work directories. Register the
libvsr slot from an authenticated account:

```sh
gh api -X POST repos/breakbadsp/libvsr/actions/runners/registration-token --jq .token | sudo bash .github/scripts/self-hosted-runner-setup.sh
```

Jobs use rootless Podman through the runner account's `docker` wrapper.
Each build uses at most two compiler processes on the shared six-CPU host.
