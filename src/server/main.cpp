/**
 * @file main.cpp
 * @brief Scalable UDP echo server.
 *
 * The server creates one listening UDP socket per CPU (for both IPv4 and
 * IPv6), affinitizes sockets and worker threads to processors, and uses
 * IO Completion Ports to efficiently process incoming datagrams and echo
 * responses back to clients.
 *
 * @copyright Copyright (c) 2025 LinuxUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

// Scalable UDP Echo Server
// - Opens a listening socket per CPU core
// - Uses SIO_CPU_AFFINITY to affinitize each socket
// - Uses an IO Completion Port per listening socket
// - Services each IOCP using an affinitized thread

#include <sys/epoll.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <numeric>
#include <syncstream>
#include <thread>

#include "common/arg_parser.hpp"
#include "common/socket_utils.hpp"

// Global flag for shutdown; set to true to request orderly termination.
std::atomic<bool> g_shutdown{false};
// Global verbose flag; enable more verbose runtime output when true.
std::atomic<bool> g_verbose{false};
// If true, reply synchronously via `sendto` instead of posting overlapped sends.
std::atomic<bool> g_sync_reply{false};

/**
 * @brief Signal handler that requests shutdown.
 */
void signal_handler(int) {
    g_shutdown.store(true);
}

/**
 * @brief Per-worker context for server-side processing.
 *
 * Holds the listening socket, IOCP handle, a worker thread and basic
 * counters for received/sent packets and bytes.
 */
struct server_worker_context {
    /// Logical processor id this worker is affinitized to.
    uint32_t processor_id;
    /// The UDP socket owned by the worker.
    unique_fd socket;
    /// IO Completion Port associated with the socket.
    unique_fd epoll_fd;
    /// Worker thread (jthread for cooperative cancellation support).
    std::jthread worker_thread;
    /// Counters and statistics.
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> bytes_sent{0};
};

/**
 * @brief Worker thread entrypoint for the server.
 *
 * Pins the thread, posts initial receives, and loops processing IOCP
 * completions for receives and sends. Received datagrams are echoed back
 * to the sender in this example.
 */
void worker_thread_func(server_worker_context* ctx) try {
    // Set thread affinity to match socket affinity
    set_thread_affinity(ctx->processor_id);

    if (g_verbose.load())
        std::osyncstream(std::cout) << std::format("[CPU {}] Worker started\n", ctx->processor_id);

    std::vector<epoll_event> events(OUTSTANDING_OPS);
    // Reuse a single receive buffer to avoid repeated allocations.
    std::vector<uint8_t> buffer;
    sockaddr_storage remote_addr;
    int remote_addr_len = 0;
    while (!g_shutdown.load()) {
        // Poll epoll_fd for completions

        int n_events =
            ::epoll_wait(ctx->epoll_fd.get(), events.data(), events.size(), EPOLL_TIMEOUT_MS);
        if (n_events == -1) {
            if (errno == EINTR) {
                continue;  // interrupted by signal
            }
            throw socket_exception(std::format("epoll_wait failed: {}", std::strerror(errno)));
        }

        for (int i = 0; i < n_events; ++i) {
            epoll_event& ev = events[i];

            // Handle urgent error/hangup conditions
            if (ev.events & (EPOLLERR | EPOLLHUP)) {
                if (g_verbose.load()) {
                    unsigned int evflags = ev.events;
                    std::osyncstream(std::cerr)
                        << std::format("[CPU {}] epoll reported error/hangup: events=0x{:x}\n",
                                       ctx->processor_id, evflags);
                }
                continue;
            }

            if (ev.events & EPOLLIN) {
                // Drain the socket: for edge-triggered epoll we must read until EAGAIN
                while (true) {
                    bool would_block = post_recv(ctx->socket, buffer, remote_addr, remote_addr_len);
                    if (would_block) {
                        break;  // no more data for now
                    }

                    // Process received datagram: echo it back
                    ctx->packets_received.fetch_add(1);
                    ctx->bytes_received.fetch_add(buffer.size());

                    if (g_sync_reply.load()) {
                        // Synchronous reply using sendto
                        send_sync(ctx->socket, reinterpret_cast<const char*>(buffer.data()),
                                  buffer.size(), reinterpret_cast<const sockaddr*>(&remote_addr),
                                  remote_addr_len);
                        ctx->packets_sent.fetch_add(1);
                        ctx->bytes_sent.fetch_add(buffer.size());
                    } else {
                        // Asynchronous reply using post_send (currently performs non-blocking send)
                        bool send_would_block = post_send(
                            ctx->socket, buffer, buffer.size(),
                            reinterpret_cast<const sockaddr*>(&remote_addr), remote_addr_len);
                        if (!send_would_block) {
                            ctx->packets_sent.fetch_add(1);
                            ctx->bytes_sent.fetch_add(buffer.size());
                        } else if (g_verbose.load()) {
                            std::osyncstream(std::cerr) << std::format(
                                "[CPU {}] send would block, dropping packet\n", ctx->processor_id);
                        }
                    }
                }
            }
        }
    }

    if (g_verbose.load())
        std::osyncstream(std::cout) << std::format(
            "[CPU {}] Worker shutting down. Stats: recv={}, sent={}, "
            "bytes_recv={}, bytes_sent={}\n",
            ctx->processor_id, ctx->packets_received.load(), ctx->packets_sent.load(),
            ctx->bytes_received.load(), ctx->bytes_sent.load());
} catch (const std::exception& ex) {
    std::osyncstream(std::cerr) << std::format("[CPU {}] Worker thread exception: {}\n",
                                               ctx->processor_id, ex.what());
    // Shutdown on unhandled exception
    g_shutdown.store(true);
} catch (...) {
    std::osyncstream(std::cerr) << std::format("[CPU {}] Worker thread unknown exception\n",
                                               ctx->processor_id);
    // Shutdown on unhandled exception
    g_shutdown.store(true);
}

/**
 * @brief Print usage/help text to stdout.
 */
void print_usage(const char* program_name) {
    std::cout
        << "Usage: " << program_name << " [options]\n"
        << "Options:\n"
        << "  --port, -p <port>         - UDP port to listen on (default: 7)\n"
        << "  --duration, -d <seconds>  - Run for N seconds then exit (0 = unlimited)\n"
        << "  --cores, -c <n>           - Number of cores to use (default: all available)\n"
        << "  --recvbuf, -b <bytes>     - Socket receive buffer size in bytes (default: "
           "4194304 = 4MB)\n"
        << "  --sync-reply, -s          - Reply synchronously using sendto (default: async IO)\n"
        << "  --verbose, -v             - Enable verbose logging (default: minimal)\n"
        << "  --help, -h                - Show this help\n";
}

/**
 * @brief Program entry point for the server.
 *
 * Parses options, initializes Winsock, creates worker contexts for IPv4/IPv6
 * on each CPU, starts threads and prints aggregated statistics on shutdown.
 */
int main(int argc, char* argv[]) try {
    ArgParser parser;
    parser.add_option("verbose", 'v', "0", false);
    parser.add_option("port", 'p', "7", true);  // Note: The IANA-assigned port for echo is 7
    parser.add_option("duration", 'd', "0", true);
    parser.add_option("cores", 'c', "0", true);
    parser.add_option("recvbuf", 'b', "4194304", true);
    parser.add_option("sync-reply", 's', "0", false);
    parser.add_option("help", 'h', "0", false);
    parser.parse(argc, argv);

    if (parser.is_set("help")) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string port_str = parser.get("port");
    const std::string cores_str = parser.get("cores");
    const std::string recvbuf_str = parser.get("recvbuf");
    const std::string duration_str = parser.get("duration");
    const std::string verbose_str = parser.get("verbose");
    const std::string sync_reply_str = parser.get("sync-reply");
    if (!verbose_str.empty() && verbose_str != "0") {
        g_verbose.store(true);
    }
    if (!sync_reply_str.empty() && sync_reply_str != "0") {
        g_sync_reply.store(true);
    }

    if (port_str.empty()) {
        throw std::invalid_argument("Port number is required");
    }

    char* endptr = nullptr;
    long port_l = std::strtol(port_str.c_str(), &endptr, 10);
    if (endptr == port_str.c_str() || port_l <= 0 || port_l > 65535) {
        throw std::invalid_argument("Invalid port number");
    }
    int port = static_cast<int>(port_l);

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;
    if (!cores_str.empty()) {
        int requested = static_cast<int>(std::strtol(cores_str.c_str(), nullptr, 10));
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
    }

    // Parse receive buffer size
    int recvbuf = 4194304;  // default 4MB
    if (!recvbuf_str.empty()) {
        long v = std::strtol(recvbuf_str.c_str(), nullptr, 10);
        if (v > 0) recvbuf = static_cast<int>(v);
    }

    // Parse optional duration (seconds)
    int duration_sec = 0;
    if (!duration_str.empty()) {
        long d = std::strtol(duration_str.c_str(), nullptr, 10);
        if (d > 0) duration_sec = static_cast<int>(d);
    }

    std::cout << std::format("Scalable UDP Echo Server\n");
    std::cout << std::format("Port: {}\n", port);
    std::cout << std::format("Available processors: {}\n", num_processors);
    std::cout << std::format("Using {} worker(s)\n", num_workers);

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create worker contexts
    std::vector<std::unique_ptr<server_worker_context>> workers;

    // Helper to create and initialize a single worker context for a given CPU id.
    auto create_worker = [&](uint32_t cpu_id,
                             int address_family) -> std::unique_ptr<server_worker_context> {
        auto ctx = std::make_unique<server_worker_context>();
        ctx->processor_id = cpu_id;

        ctx->socket = create_udp_socket(address_family);

        set_socket_cpu_affinity(ctx->socket, static_cast<uint16_t>(cpu_id));

        // Increase socket buffers.
        set_socket_option(ctx->socket, SOL_SOCKET, SO_RCVBUF,
                          reinterpret_cast<const char*>(&recvbuf), sizeof(recvbuf));
        set_socket_option(ctx->socket, SOL_SOCKET, SO_SNDBUF,
                          reinterpret_cast<const char*>(&recvbuf), sizeof(recvbuf));

        // Bind socket to the requested port
        bind_socket(ctx->socket, static_cast<uint16_t>(port), address_family);

        // Create IOCP and associate socket
        ctx->epoll_fd = create_epoll_and_associate(ctx->socket);

        if (g_verbose.load())
            std::osyncstream(std::cout)
                << std::format("Created socket and IOCP for CPU {}\n", cpu_id);
        return ctx;
    };

    for (uint32_t i = 0; i < num_workers; ++i) {
        // Start one worker per address family per CPU
        workers.push_back(create_worker(i, AF_INET));
        workers.push_back(create_worker(i, AF_INET6));
    }

    if (workers.empty()) {
        throw std::runtime_error("No worker contexts created");
    }

    // Start worker threads
    auto start_worker_threads = [&](std::vector<std::unique_ptr<server_worker_context>>& wks) {
        for (auto& ctx : wks) {
            ctx->worker_thread = std::jthread(worker_thread_func, ctx.get());
        }
    };

    auto close_iocps = [&](const std::vector<std::unique_ptr<server_worker_context>>& wks) {
        for (const auto& ctx : wks) {
            ctx->epoll_fd.reset();
        }
    };

    auto join_and_cleanup_workers =
        [&](const std::vector<std::unique_ptr<server_worker_context>>& wks) {
            for (const auto& ctx : wks) {
                if (ctx->worker_thread.joinable()) ctx->worker_thread.join();
            }

            for (const auto& ctx : wks) {
                ctx->socket.reset();
            }
        };

    auto print_final_stats = [&](const std::vector<std::unique_ptr<server_worker_context>>& wks) {
        uint64_t total_recv = 0, total_sent = 0, total_bytes_recv = 0, total_bytes_sent = 0;
        for (const auto& ctx : wks) {
            total_recv += ctx->packets_received.load();
            total_sent += ctx->packets_sent.load();
            total_bytes_recv += ctx->bytes_received.load();
            total_bytes_sent += ctx->bytes_sent.load();
        }

        std::osyncstream(std::cout) << std::format("\nFinal Statistics:\n");
        std::osyncstream(std::cout) << std::format("  Total packets received: {}\n", total_recv);
        std::osyncstream(std::cout) << std::format("  Total packets sent: {}\n", total_sent);
        std::osyncstream(std::cout)
            << std::format("  Total bytes received: {}\n", total_bytes_recv);
        std::osyncstream(std::cout) << std::format("  Total bytes sent: {}\n", total_bytes_sent);
    };

    start_worker_threads(workers);

    std::osyncstream(std::cout) << std::format(
        "\nServer running on port {}. Press Ctrl+C to stop.\n\n", port);

    // Optional timed shutdown
    std::thread duration_thread;
    if (duration_sec > 0) {
        duration_thread = std::thread([duration_sec]() {
            std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
            g_shutdown.store(true);
        });
    }

    // RPS printer thread: aggregate per-worker `packets_received` once per second
    std::thread rps_thread([&workers]() {
        uint64_t prev_total = 0;
        while (!g_shutdown.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            uint64_t total_recv = std::accumulate(
                workers.begin(), workers.end(), 0ULL,
                [&](uint64_t sum, const std::unique_ptr<server_worker_context>& ctx) {
                    return sum + ctx->packets_received.load(std::memory_order_relaxed);
                });

            uint64_t rps = (total_recv >= prev_total) ? (total_recv - prev_total) : 0;
            prev_total = total_recv;

            std::osyncstream(std::cout) << std::format("[RPS] {} req/s\n", rps);
        }
    });

    // Wait for shutdown signal (main thread sleeps while RPS thread runs)
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Join the RPS thread so it exits cleanly before we teardown workers
    if (rps_thread.joinable()) rps_thread.join();

    // Join duration thread if running
    if (duration_thread.joinable()) duration_thread.join();

    std::osyncstream(std::cout) << "\nShutting down...\n";

    // Close IOCPs to wake up worker threads, then join and cleanup
    close_iocps(workers);
    join_and_cleanup_workers(workers);

    // Print final stats and cleanup winsock
    print_final_stats(workers);

    return 0;
} catch (const socket_exception& ex) {
    std::osyncstream(std::cerr) << std::format("Socket exception in main: {}\n", ex.what());
    return 1;
} catch (const std::exception& ex) {
    std::osyncstream(std::cerr) << std::format("Exception in main: {}\n", ex.what());
    return 1;
} catch (...) {
    std::osyncstream(std::cerr) << "Unknown exception in main\n";
    return 1;
}