# Host contract

The library replicates an application only if the application and its host
obey these rules. The library cannot enforce most of them. The simulator and
the checkpoint comparison detect some violations.

## The state machine

Implement `vsr::StateMachine` (`include/vsr/interfaces.hpp`).

1. **`Execute` is deterministic.** The same operations in the same order
   give the same replies and the same state on each replica. In `Execute`:
   - Use `Operation::timestamp_ns_` in place of the clock.
   - For a random number, seed a generator from `op_` and `timestamp_ns_`.
   - Do not read files, the network, the environment, or a thread identifier.
   - Do not let the result depend on memory addresses or on the iteration
     order of a hash table.
2. **`Execute` has no external effect.** A replica executes an operation
   again after a restart, and each replica executes it. To send an email or
   a payment, record the intent in the state, and let one separate process
   do it with its own retry and deduplication.
3. **`Execute` gives a reply for each body**, also one that it cannot
   parse. It must not stop the process: the same body would stop every replica.
4. **`Snapshot` bytes are equal on each replica** for the same state. Write
   containers in a fixed order. Write no pointers.
5. **`Restore(Snapshot())` gives the same state.**
6. **A new version of the application executes old operations in the same
   way.** Replicas must not run different versions at the same time
   (rolling upgrades are not in version 1).

A violation of 1, 4, or 5 makes the checkpoints differ. The replica that
sees the difference halts with `HaltReason::Divergence`.

## The host loop

```
replica = Replica::Open(config, storage, bus, state_machine)
loop:
    for each message that arrived:  replica.OnMessage(bytes)
    for each request of an own client protocol:  replica.Submit(client, request, session, body)
    each tick (for example 10 ms):  replica.Tick(wall_clock_ns)
    replica.Flush()
    stop if replica.GetStatus() == Status::Halted
```

- Call the replica from one thread.
- Call `Flush` after each batch. No acknowledgement and no reply leaves
  before it. One sync covers the batch.
- `Storage::Sync` must be a real barrier (`fdatasync`, with the write cache
  of the device flushed). If it lies, the durability promise is gone.
- After `Sync` or `Write` returns an error, the replica halts. Start the
  process again; do not continue with the same replica object.
- `MessageBus` can drop, delay, repeat, or reorder messages. It must not
  change them: a changed message fails its checksum and is dropped.
- Each replica needs its own storage. Never copy the storage of one replica
  to another replica.

## Clients

- A client has an identifier that is not zero. Before its first operation
  it registers: request number 0 with an empty body. The cluster then keeps
  a session for it. Registration uses session 0. Its reply gives `session_`,
  the committed registration operation number. Each later request must
  carry that session number.
- Operations have request numbers 1, 2, 3, and so on. The identifier and
  the session number and the request number identify one logical operation.
- A client has at most one request in progress.
- After a timeout the outcome is not known. Send the **same** request
  number again. The operation then executes at most once, and the client
  gets the stored reply.
- The cluster holds `Config::clients_max_` sessions. When a new client
  registers and the table is full, the session with the oldest commit is
  removed. A request of a client without a session does not execute. The
  client gets an `Eviction` message (`SubmitResult::NoSession`), and must
  register again with a **new** identifier. The outcome of a request that
  was in progress at that time cannot be learned. Set `clients_max_`
  above the number of clients that are active at one time.
- A client that restarts without its last request number takes a new
  identifier and registers.
- Ignore an `Eviction` message with a view older than the view of a reply
  that the client already has. A replica that is behind does not know new
  sessions.

`vsr::ClientCore` (`include/vsr/client_core.hpp`) does all of this, without
input and output. `vsr::io::Client` gives it sockets and a clock. A host
with its own network can run the core directly.

## Limits

`Config` (`include/vsr/config.hpp`) fixes all sizes at start. The storage
file records them, and a replica does not open a file with other values.
The replica allocates all memory in `Open`.

## Adapter work for an existing application (for example `mex`)

1. Encode each command of the engine as bytes: the request body.
2. In `Execute`: decode the body, call the engine, encode the result as the
   reply. Replace clock reads in the engine with the given timestamp.
3. `Snapshot` and `Restore`: write and read the full engine state in a
   fixed order.
4. Move effects that leave the process (market data, drop copies) out of
   `Execute`, behind the committed replies.
5. Keep the gateway and its protocol. Give each gateway session a client
   identifier, register it, and call `Replica::Submit` with a request
   number for each command.
