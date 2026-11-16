# Use libvsr in another project

This guide is for a developer or an AI agent who adds libvsr to a project.
The rules are in [host-contract.md](host-contract.md). Read them before you
write code. This guide tells you which path to use and what work to do.

## When to use it

Use libvsr when one deterministic, in-memory application must keep one
state on 3 to 7 machines, and must survive crashes and power loss.
Each operation is ordered, executed at most once, and durable on a
majority before the client gets the reply.

Do not use it when:

- The state is larger than one snapshot buffer in memory
  (`Config::snapshot_bytes_max_`, 16 MiB by default).
- Reads must be fast and local. Each read goes through the log.
- Replicas run on a network that you do not trust (no Byzantine defence).
- You must add or remove replicas while the cluster runs.
- You need production maturity now. Version 1 is not yet proven.

## Requirements

| Item | Requirement |
|------|-------------|
| Compiler | GCC 14 or Clang 18, or later. C++23. |
| Build | CMake 3.25 and Ninja. GoogleTest only for the tests of libvsr. |
| Platform | `vsr` (the protocol): any platform. `vsr_io` (files, TCP, runner): Linux, IPv4 addresses only. |
| Cluster | 1 to 7 replicas (`vsr::kReplicasMax`). Use 3 or 5. A majority must be up. |
| Storage | One file or device for each replica. `Sync` must be a real barrier (`fdatasync` with the device cache flushed). |
| Threads | Call one replica from one thread. |
| Application | Deterministic `Execute`, `Snapshot`, and `Restore`. See below. |

## Add it to the build

There is no install target. Add the source tree to your project:

```cmake
include(FetchContent)
FetchContent_Declare(libvsr GIT_REPOSITORY <libvsr URL> GIT_TAG <commit>)
set(VSR_BUILD_TESTS OFF CACHE BOOL "" FORCE)     # removes the GoogleTest need
set(VSR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libvsr)

target_link_libraries(my_server PRIVATE vsr_io)  # or only vsr, see path B
```

`add_subdirectory(third_party/libvsr)` with the same options also works.
Pin a commit. The storage format and the wire format can change before
version 1 is complete.

## Select a path

| Path | Use it when | You write |
|------|-------------|-----------|
| A: `vsr::io::Server` and `vsr::io::Client` | You start new, or TCP and one file for each replica are sufficient. | The state machine and `main`. |
| B: `vsr::Replica` | You have your own network, storage, event loop, or client protocol (for example an existing gateway). | The state machine, the host loop, and adapters for `Storage` and `MessageBus` if the defaults do not fit. |

### Path A: the runner

Copy [`examples/kv_server.cpp`](../examples/kv_server.cpp) and
[`examples/kv_client.cpp`](../examples/kv_client.cpp), and replace
`kv::StateMachine` with your state machine.

```cpp
vsr::io::ServerOptions options;
options.config_.cluster_ = 1;                // equal on each replica and client
options.config_.replica_count_ = 3;
options.config_.replica_ = my_index;         // 0, 1, or 2
options.addresses_ = {"10.0.0.1:3000", "10.0.0.2:3000", "10.0.0.3:3000"};
options.data_path_ = "/var/lib/my_app/replica.vsr";  // made and formatted if missing

MyStateMachine state_machine;                // initial state
auto server = vsr::io::Server::Create(std::move(options), state_machine);
auto result = server->Run();                 // returns a HaltReason if the replica halts
```

Client side:

```cpp
auto client = vsr::io::Client::Create({.cluster_ = 1, .addresses_ = addresses});
auto reply = client->Request(body, std::chrono::seconds{2});
while (!reply && reply.error() == vsr::io::ClientError::Timeout)
{
  reply = client->Resume(std::chrono::seconds{2});  // same request, at most once
}
```

### Path B: your own host

Implement the interfaces in
[`include/vsr/interfaces.hpp`](../include/vsr/interfaces.hpp) that the
defaults do not cover. `vsr::io::FileStorage` is a default `Storage`.
Then run the loop in
[host-contract.md, "The host loop"](host-contract.md#the-host-loop):
`Format` once, `Replica::Open`, then `OnMessage`, `Submit`, `Tick`, and
`Flush` for each batch. Your client protocol must do the registration and
retry rules in [host-contract.md, "Clients"](host-contract.md#clients).

## Adapt an application

Do these steps in this sequence:

1. **Define the request body and the reply as bytes.** Use one versioned
   binary format. A body can hold several commands to do more work in one
   round trip.
2. **Make `Execute` deterministic.** Find each clock read, random number,
   file or network read, thread identifier, pointer value, and hash table
   iteration in the code that `Execute` calls. Replace the clock with
   `Operation::timestamp_ns_`. Seed random numbers from `op_` and
   `timestamp_ns_`. Use ordered containers.
3. **Remove external effects from `Execute`.** Each replica executes each
   operation, and again after a restart. Record the intent in the state.
   Let one separate process do the effect from the committed replies, with
   its own deduplication.
4. **Reply to each body.** A body that does not parse gets an error reply
   and changes nothing. Never throw from `Execute` or stop the process:
   the same body stops each replica.
5. **Write `Snapshot` and `Restore`.** Write the full state in a fixed
   order, with no pointers. Equal state must give equal bytes on each
   replica. `Restore(Snapshot())` must give the same state.
6. **Size `Config`.** Set `body_bytes_max_` above the largest request and
   reply, `snapshot_bytes_max_` above the largest state, and `clients_max_`
   above the number of clients that are active at one time. The storage
   file records these values. To change them, you must format new storage.
7. **Test determinism yourself.** The libvsr simulator uses its own state
   machine. Write a test that runs two instances of your state machine on
   the same operation sequence, and compares replies and snapshot bytes.
   Also test `Restore(Snapshot())` at random points.
8. **Monitor halts.** A replica that halts ignores all input. Log the
   `HaltReason`, alert, and start the process again. `Divergence` means
   step 2 or 5 is wrong.

## Limitations (version 1)

- Disk corruption is detected, not repaired. The replica halts.
- No reconfiguration and no rolling upgrades. Stop all replicas to upgrade,
  and the new version must execute old operations in the same way.
- Timestamps come from the clock of the primary. A wrong clock gives wrong
  timestamps.
- Each read is a full replicated operation. There are no local or lease reads.
- A checkpoint writes the full state. Large state makes checkpoints slow.
- A sync blocks the replica thread (no `io_uring`, no `O_DIRECT`).
- A request that arrives when the pipeline is full is dropped. The client
  sends it again.
- XXH64 checksums detect accidents, not attacks. Use a trusted network.
- `vsr_io` is Linux and IPv4 only. No host names.

The full list and the reasons are in
[design-decisions.md](design-decisions.md#not-in-version-1-todo).

## Checklist for an AI agent

- [ ] Read `docs/host-contract.md` and `include/vsr/interfaces.hpp`.
- [ ] Select path A or path B, and say why.
- [ ] `Execute` reads only its arguments and the state. No clock, random,
      I/O, pointer, or hash order.
- [ ] `Execute` has no external effect, and replies to each body.
- [ ] `Snapshot` writes in a fixed order. `Restore(Snapshot())` round-trips.
- [ ] `Config` is equal on each replica except `replica_`, and sized for
      the largest body, state, and client count.
- [ ] Clients register, keep one request in progress, and retry with the
      same request number after a timeout.
- [ ] A determinism test and a snapshot round-trip test pass.
- [ ] A three-replica run survives a kill of the primary.
