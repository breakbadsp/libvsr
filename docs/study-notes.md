# VSR study notes and feasibility

This is an initial research assessment. It does not select APIs, a language,
a storage format, or a runtime.

## What VSR provides

VSR agrees on the order of operations for a replicated service. For the basic
2012 protocol, `2f + 1` replicas tolerate up to `f` crash failures. A quorum
contains `f + 1` replicas.

The primary orders each request in its log and sends `PREPARE`. After `f`
backup acknowledgements, it commits, executes in order, and replies. Backups
learn the commit position. Client identifiers and request numbers suppress
duplicate execution.

A view identifies a primary's term. When that primary appears to fail, a
view change collects quorum information. It selects a log using protocol
rules that preserve committed operations. Selecting the longest log alone
is insufficient. A recovering replica must obtain adequate state before
participating again.

Safety means committed history remains consistent. Liveness means requests
can finish. A reachable quorum is necessary; progress also needs sufficiently
stable communication and a functioning primary. Safety can hold while
progress stops. The paper's memory-based protocol does not survive loss of
all replica state.

Source: [2012 paper, §§2–4 and §8](papers/vr-revisited-2012.pdf).

## The application must fit the model

Replicas must begin with equivalent state and execute deterministic commands.
The same ordered inputs must produce the same state and results.
Schneider's [tutorial](papers/state-machine-tutorial-1990.pdf) explains this
model and treats output devices separately.

For this project, that implies several integration constraints:

- Local clock reads, random values, and external query results cannot vary
  independently during command execution. Inputs that affect results need
  agreement or another explicit rule.
- Replicating a command does not make an external action occur once.
  For example, a payment API call needs a separate retry and deduplication contract.
- An application fault can occur on every replica. Replication alone does not
  correct that fault.
- Reads need an explicit consistency contract. An arbitrary backup can lag.
- A client timeout can leave the outcome unknown. The client needs a way to
  retry or query the same operation without creating a new logical operation.

These are assessment conclusions from the state machine model. They explain
why “any mission critical software” is too broad as an initial promise.
A more credible objective is software that can satisfy a documented replication
and recovery contract.

## Why TigerBeetle requires separate study

TigerBeetle's [VSR documentation](https://github.com/tigerbeetle/tigerbeetle/blob/68958eb757747d0fc96a79d1f9c2fa6766b67b48/docs/internals/vsr.md)
describes changes beyond the paper. It persists VSR state in a superblock
instead of using the paper's recovery protocol. It also describes write-ahead
log repair, state synchronization, and separate replication and view-change
quorums. Do not apply the basic paper's quorum formula to every TigerBeetle
configuration.

Its [simulation documentation](https://github.com/tigerbeetle/tigerbeetle/blob/68958eb757747d0fc96a79d1f9c2fa6766b67b48/docs/internals/vopr.md)
describes running production code with controlled clocks, network operations,
and disk operations. A seed and Git commit allow failure replay. The simulator
injects network and storage faults and checks safety and liveness.

Storage faults deserve their own failure model.
[Protocol-Aware Recovery](papers/protocol-aware-recovery-2018.pdf) demonstrates
that consensus-based storage systems can lose data or become unavailable
under storage faults. Its recovery method uses protocol knowledge. A correct
crash-failure protocol alone does not establish safe recovery from corruption.

## How difficult is a reusable library?

My assessment: a focused library is feasible. A library that deserves trust
in mission-critical systems is a major, continuing systems project.
The paper's algorithms are only part of that work.

The difficult part of generality is the contract with each application and
runtime. A caller must not acknowledge durable storage too early, restore
incompatible state, or execute a command differently after an upgrade.
A generic interface can expose these obligations, but cannot remove them.

The scope changes substantially with the promise:

- A learning implementation needs the basic protocol and clear examples.
- A reusable protocol component also needs precise host obligations and
  verification with more than one application.
- A production library also needs crash recovery, bounded resource use,
  safe state transfer, operational diagnostics, and supported upgrade paths.
- A mission-critical claim also needs independent review, extensive failure
  testing, deployment evidence, and a maintained incident response process.

This is a scope assessment, not a delivery estimate. There is no sound schedule
yet. The team, supported platforms, workload, failure model, and acceptance
criteria remain unknown. A successful prototype would not establish production
readiness.

## Questions to resolve before design

1. Which failures are covered: process crashes, machine restarts, total power
   loss, disk corruption, or malicious participants? Basic VSR is not a
   Byzantine fault-tolerant protocol.
2. What makes an acknowledged operation durable? Which failures may happen
   together, and what state must survive them?
3. What must an application provide for deterministic execution, snapshots,
   restore, client sessions, and external effects?
4. Which reads need linearizability? What can a disconnected client observe?
5. Must the first release support membership changes, rolling upgrades,
   multiple replication groups, and multiple storage or network systems?
6. Which independent checks will establish safety? Under which repaired
   conditions must the system resume progress?

## Study exercises before implementation

Use three replicas and trace these cases against the 2012 paper. Record
messages, log entries, view numbers, and client-visible outcomes:

1. A successful request, followed by a retry after its reply is lost.
2. A primary failure after replication but before the client receives a reply.
3. A partition that separates one replica from the other two, then heals.
4. An obsolete primary that returns with delayed messages.
5. A replica that loses volatile state and attempts to rejoin.

For each case, explain why a committed operation survives, why a duplicate
does not execute twice, and when progress can resume. Then compare recovery
with TigerBeetle's documented approach. These exercises can expose gaps in
understanding before they become design assumptions.
