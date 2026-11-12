# Verification

## What runs

| Check | Command | What it shows |
|-------|---------|---------------|
| Unit and integration tests | `ctest --preset gcc` (also `clang`, `gcc-san`, `clang-san`) | The host contract of one replica, the formats, the journal, and a real three-replica cluster on TCP and files. |
| Simulation | `./build/gcc/sim/vsr_sim <first seed> <seed count> <replicas> [chaos ticks]` | Safety with faults, and recovery after the faults stop. |
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

Checks during the run:

- Each replica executes operations in order with no gap.
- Each replica executes the same operation at each operation number, and
  has the same state after it.
- Timestamps increase.
- No request executes more than once.
- A client reply names the right operation and has the right body, and the
  operation committed after the request was sent.
- When an operation first executes, a quorum holds it on durable storage.
- A replica sends `PrepareOk` or `DoViewChange` only when the log and the
  view that the message promises are durable.
- No correct replica counts a protocol violation or an invalid message.

Checks after the faults stop: each client request gets its reply, all
replicas are `Normal` in one view with one primary, and all replicas
executed all operations.

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

## Known gaps

- The simulator does not inject disk corruption. That failure type is not
  in version 1.
- The TCP runner and the client have integration tests on one machine.
  They have no fault injection on real networks or disks (Jepsen-style).
- No test runs more than a few minutes. A long soak (millions of seeds)
  is a task for CI or a dedicated machine.

## AlmaLinux CI

PR checks use a prepared AlmaLinux 8 container on the Omarchy `libvsr`
runner. Fork PRs use GitHub-hosted runners. Both GCC and Clang run tests
and 300 simulation seeds for three and five replicas, with and without
ASan/UBSan. A separate job fuzzes the instrumented decoder for 60 seconds.
Each job keeps its source mount read-only and builds in a disposable
container. Build logs, package versions, and CTest logs are CI artifacts.

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
