<img src="assets/icon.svg" width="64" height="64" alt="">

# libvsr

A generic Viewstamped Replication (VSR) library in C++23, inspired by
TigerBeetle. It keeps one deterministic application in the same state on
several machines, and it survives crashes and power loss.

The library replicates opaque bytes. The application supplies three
functions: execute an operation, write a snapshot, and restore a snapshot.

**Status: version 1 in development. Not yet proven in production.** Read
[what is not in version 1](docs/design-decisions.md#not-in-version-1-todo).

## Use it

The short path: `vsr::io::Server` runs one replica with file storage and
TCP. `vsr::io::Client` sends requests. See [`examples/`](examples/).

```sh
cmake --preset gcc && cmake --build --preset gcc
./build/gcc/examples/kv_server 0 /tmp/kv0.vsr 127.0.0.1:3000 127.0.0.1:3001 127.0.0.1:3002 &
./build/gcc/examples/kv_server 1 /tmp/kv1.vsr 127.0.0.1:3000 127.0.0.1:3001 127.0.0.1:3002 &
./build/gcc/examples/kv_server 2 /tmp/kv2.vsr 127.0.0.1:3000 127.0.0.1:3001 127.0.0.1:3002 &
./build/gcc/examples/kv_client name=libvsr 127.0.0.1:3000 127.0.0.1:3001 127.0.0.1:3002
```

A host with its own network, storage, or client protocol uses
`vsr::Replica` directly and implements the interfaces in
[`include/vsr/interfaces.hpp`](include/vsr/interfaces.hpp).

## Read

- [Host contract](docs/host-contract.md): what the application and the host must do.
- [Design decisions](docs/design-decisions.md): what was selected, why, and the TODO list.
- [Verification](docs/verification.md): the simulator, its checks, and how to run them.
- [Papers and study notes](docs/README.md): the research behind the design.

## Build and test

Requirements: GCC 14 or Clang 18 (or later), CMake 3.25, Ninja, GoogleTest.

```sh
cmake --preset gcc && cmake --build --preset gcc && ctest --preset gcc
./build/gcc/sim/vsr_sim 1 300 3     # 300 simulation seeds, three replicas
```

Presets: `gcc`, `clang`, `gcc-san`, `clang-san` (AddressSanitizer and
UndefinedBehaviorSanitizer).
