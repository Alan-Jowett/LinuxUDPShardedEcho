
<!-- SPDX-License-Identifier: MIT -->

# LinuxUDPShardedEcho - A Scalable Echo Server Demo (Linux / epoll)

This repository contains a high-performance UDP echo server and client optimized for modern
Linux systems. The implementation demonstrates how to scale UDP packet processing across CPU
cores using one socket (or a small set of sockets) per worker combined with an `epoll`-based
event loop and worker threads affinitized to individual CPU cores.

Implements RFC 862 - Echo Protocol: https://www.rfc-editor.org/rfc/rfc862


## Requirements

- Linux (kernel 4.x or later recommended)
- A C++20 capable compiler (`g++` or `clang++`)
- CMake 3.20 or later
- Build tools: `make` or Ninja


## Building

```bash
# Create build directory
mkdir -p build
cd build

# Configure with CMake (out-of-source build)
cmake ..

# Build
cmake --build . --config Release
```

The produced binaries are `echo_server` and `echo_client` and are located in the `build/`
directory after a successful build.


## Runtime Behavior & Usage

Both the server and client are command-line programs. They accept similar worker/core,
buffer, and runtime configuration options. Run either binary with `--help` to see the full list
of supported flags.

Server example:
```bash
./echo_server --port 5000 --cores 4
```

Client example:
```bash
./echo_client --server 127.0.0.1 --port 5000 --sockets 2 --rate 20000 --cores 4 --duration 10
```

Common options of interest:
- `--port, -p <port>` : UDP port to use (server listens; client targets)
- `--cores, -c <n>` : Number of worker threads / cores to use (default: all available)
- `--sockets, -k <n>` : Number of sockets per worker (client)
- `--recvbuf, -b <bytes>` : Socket receive buffer size
- `--rate, -r <pps>` : Total packets-per-second (client)


Example:
```bash
echo_client --server 127.0.0.1 --port 5000 --sockets 4 --rate 20000 --cores 2 --duration 5
echo_client --server 192.168.1.100 --port 5000 --sockets 1 --rate 10000 --payload 1024 --cores 4 --duration 30
```


## Architecture

- One (or a small number of) UDP socket(s) per worker thread. Each worker's socket(s) are
   handled on the same CPU core to improve cache locality and reduce contention.
- An `epoll`-based event loop is used to efficiently wait for incoming datagrams and dispatch
   processing to the affinitized worker thread.
- The client can create multiple sockets per worker to increase source-port entropy when
   needed for better distribution across RX queues in the NIC and kernel.
- Per-worker statistics and lightweight estimators (TDigest / PercentileEstimator) collect
   latency and RTT metrics for reporting.


Packet format used by the examples (sequence + timestamp + payload):

```
+------------------------+------------------------+
|  Sequence Number (8B)  |  Timestamp NS (8B)     |
+------------------------+------------------------+
|                    Payload                      |
+------------------------------------------------+
```

### Key Features

- **Per-worker socket(s)**: Each worker thread owns one (or a small set of) UDP socket(s). Keeping
   socket handling local to a worker improves cache locality and reduces cross-thread contention.

- **epoll-based event loop**: Workers wait on an `epoll` instance to receive readiness notifications
   for their socket(s). `epoll` provides efficient readiness notification and scales well with many
   file descriptors.

- **Thread / CPU affinity**: Worker threads can be affinitized (pinned) to specific CPU cores to
   keep packet processing on the same core as the socket handling. This reduces cache misses and
   improves predictability under load.

- **Multiple sockets per worker (client option)**: The client can open multiple sockets per worker to
   increase source-port entropy and improve distribution across NIC RX queues and kernel RSS.

- **Batched processing & concurrency**: Workers batch reads and processing where possible and use
   per-worker data structures to avoid shared locking hot spots. The server supports both synchronous
   send paths and non-blocking/asynchronous send flows depending on runtime options.

- **Lightweight statistics and estimators**: Each worker collects metrics (counts, bytes, and latency
   samples). The code includes `TDigest` utilitiy used for percentile calculations and reporting.

- **Practical tuning knobs**: Options for socket receive buffer sizes, worker counts, number of
   sockets per worker, and packet pacing allow experiments across throughput/latency tradeoffs.


## Performance Notes

- Use RSS-capable NICs and configure RSS queues to match the number of cores/workers.
- Tune socket receive buffer sizes (`SO_RCVBUF`) to avoid drops under high throughput.
- Pin worker threads to CPU cores (the binaries include options to control affinities).


## Tests and Formatting

Run the project's formatting and static checks with the provided script:

```bash
./scripts/check-format.sh --all --fix
```

This will run `clang-format` over the repository and `cppcheck` for a lightweight static
analysis pass.


## Contributing

Contributions, bug reports, and performance tuning patches are welcome. Please follow the
contribution guidelines in `CONTRIBUTING.md`.

## License

MIT License — see `LICENSE` for details.
