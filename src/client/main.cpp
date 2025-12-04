/**
 * @file main.cpp
 * @brief Scalable UDP echo client.
 *
 * The client creates one or more worker threads per CPU. Each worker affinitizes
 * sockets and threads to a specific logical processor, posts asynchronous
 * receives, and sends UDP packets containing a sequence number and timestamp.
 * Received echoes are used to compute RTT and detect lost packets.
 *
 * @copyright Copyright (c) 2025 LinuxUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

// Scalable UDP Echo Client
// - Opens a socket per CPU core
// - Uses SIO_CPU_AFFINITY to affinitize each socket
// - Uses an IO Completion Port per socket
// - Services each epoll_fd using an affinitized thread
// - Sends UDP packets with sequence numbers
// - Tracks received packets to detect dropped packets

#include <arpa/inet.h>
#include <sys/epoll.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <syncstream>
#include <unordered_set>

#include "common/arg_parser.hpp"
#include "common/socket_utils.hpp"
#include "common/tdigest.hpp"

// Global flag for shutdown; set to true to request orderly termination.
std::atomic<bool> g_shutdown{false};
// Global verbose flag; when true, additional runtime information is logged.
std::atomic<bool> g_verbose{false};

/**
 * @brief Signal handler that requests shutdown.
 *
 * Sets `g_shutdown` to true so worker threads can exit cleanly.
 */
void signal_handler(int) {
    g_shutdown.store(true);
}

/**
 * @brief Per-worker context holding sockets, epoll_fd and statistics.
 *
 * Each worker owns one epoll_fd and one or more UDP sockets affinitized to the
 * worker's processor. The struct tracks per-worker counters for sent/recv
 * packets, outstanding sequence numbers, and RTT aggregates.
 */
struct client_worker_context {
    /// Logical processor this worker is affinitized to.
    uint32_t processor_id;
    /// UDP sockets owned by this worker.
    std::vector<unique_fd> sockets;
    /// IO Completion Port used by this worker.
    unique_fd epoll_fd;
    /// Worker thread instance.
    std::thread worker_thread;
    /// Next sequence number to use for outgoing packets.
    std::atomic<uint64_t> next_sequence{0};
    /// Counters for packets sent/received/dropped.
    std::atomic<uint64_t> packets_sent{0};
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_dropped{0};
    /// Counters for bytes sent/received and RTT aggregations.
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> total_rtt_ns{0};
    std::atomic<uint64_t> min_rtt_ns{UINT64_MAX};
    std::atomic<uint64_t> max_rtt_ns{0};

    /// Outstanding sequence numbers awaiting echo responses.
    std::unordered_set<uint64_t> outstanding_sequences;

    /// Target server address to send packets to.
    sockaddr_storage server_addr;
    /// Length of `server_addr`.
    int server_addr_len;
    /// Per-worker packet rate (packets per second) assigned from global total.
    uint64_t per_worker_rate{0};
    /// Index used to round-robin across multiple sockets.
    std::atomic<size_t> next_socket_index{0};

    std::unique_ptr<TDigest> curren_rtt_tdigest;
};

std::mutex g_worker_contexts_mutex;
std::vector<std::unique_ptr<TDigest>>
    g_worker_rtt_tdigests;  ///< Digest posted by each worker for merging.

// Packet rate limit total across all workers (packets per second, 0 = unlimited)
// Each worker will be assigned an equal share (plus remainder distribution).
uint64_t g_rate_limit = 10000;  // default total

TDigest g_overall_rtt_tdigest(100.0);  ///< Global RTT TDigest for percentile estimation

/**
 * @brief Atomically update a target to the minimum of its current value and `value`.
 */
void update_min(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load();
    while (value < current && !target.compare_exchange_weak(current, value)) {
        // current is updated by compare_exchange_weak
    }
}

/**
 * @brief Atomically update a target to the maximum of its current value and `value`.
 */
void update_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load();
    while (value > current && !target.compare_exchange_weak(current, value)) {
        // current is updated by compare_exchange_weak
    }
}
/**
 * @brief Merge per-worker RTT TDigests into the global RTT TDigest.
 */
void merge_tdigest() {
    std::lock_guard<std::mutex> lock(g_worker_contexts_mutex);
    for (auto& worker_tdigest : g_worker_rtt_tdigests) {
        if (worker_tdigest) {
            g_overall_rtt_tdigest.merge(*worker_tdigest);
        }
    }
    g_worker_rtt_tdigests.resize(0);
    g_overall_rtt_tdigest.compress();
}

/**
 * @brief Thread function that periodically merges per-worker RTT TDigests into the global digest.
 */
void tdigest_merge_thread() {
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Merge per-worker RTT TDigests into global
        merge_tdigest();
    }
}

/**
 * @brief Helper function to post RTT sample to per-worker TDigest and handle digest rotation.
 *
 * @param[in,out] current_digest The current per-worker TDigest.
 * @param[in] rtt_ns The RTT sample in nanoseconds.
 */
void post_rtt(std::unique_ptr<TDigest>& current_digest, uint64_t rotate_threshold,
              uint64_t rtt_ns) {
    if (!current_digest) {
        current_digest = std::make_unique<TDigest>(100.0);
    }
    current_digest->add(static_cast<double>(rtt_ns) / 1'000'000.0);  // convert to ms

    if (current_digest->total_weight() >= static_cast<double>(rotate_threshold)) {
        // Post to global for merging
        {
            std::lock_guard<std::mutex> lock(g_worker_contexts_mutex);
            g_worker_rtt_tdigests.push_back(std::move(current_digest));
        }
        current_digest = nullptr;
    }
}

/**
 * @brief Worker thread entrypoint.
 *
 * Each worker affinitizes the thread, posts a pool of receive operations,
 * sends packets according to the per-worker rate quota, and processes epoll_fd
 * completions for sends and receives.
 *
 * @param ctx Pointer to the worker context owned by the main thread.
 * @param payload_size Size in bytes of the application payload (not including header).
 */
void worker_thread_func(client_worker_context* ctx, size_t payload_size) try {
    // Set thread affinity to match socket affinity
    set_thread_affinity(ctx->processor_id);

    std::vector<uint8_t> send_buffer;
    send_buffer.resize(HEADER_SIZE + payload_size);
    std::vector<uint8_t> recv_buffer;
    recv_buffer.resize(HEADER_SIZE + payload_size);

    // Rate limiting: each worker maintains a quota = elapsed_time * per_worker_rate
    auto start_time = std::chrono::steady_clock::now();
    std::vector<epoll_event> events(ctx->sockets.size());

    while (!g_shutdown.load()) {
        // Calculate elapsed time and allowed sends
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time).count();
        uint64_t allowed_sends = 0;
        if (ctx->per_worker_rate == 0) {
            allowed_sends = UINT64_MAX;  // unlimited
        } else {
            allowed_sends =
                (elapsed_ns * ctx->per_worker_rate) / 1'000'000'000;  // per_worker_rate is pps
        }

        // Send packets up to allowed sends
        while (ctx->packets_sent.load() < allowed_sends) {
            // Select next socket in round-robin fashion

            uint64_t seq = ctx->next_sequence.fetch_add(1);

            // Prepare packet header
            packet_header header;
            header.sequence_number = htobe64(seq);
            header.timestamp_ns = htobe64(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count()));

            // Copy header into send buffer
            std::memcpy(send_buffer.data(), &header, sizeof(header));

            bool sent = false;

            size_t socket_index = ctx->next_socket_index.fetch_add(1) % ctx->sockets.size();

            for (size_t i = 0; i < ctx->sockets.size(); ++i) {
                socket_index = (socket_index + 1) % ctx->sockets.size();
                unique_fd& sock = ctx->sockets[socket_index];

                // Post send
                bool would_block =
                    post_send(sock, send_buffer, payload_size,
                              reinterpret_cast<sockaddr*>(&ctx->server_addr), ctx->server_addr_len);
                if (would_block) {
                    // Socket would block; break to wait for epoll_fd events
                    continue;
                }
                sent = true;
                break;
            }

            if (!sent) {
                // All sockets would block; break to wait for epoll_fd events
                break;
            }

            // Track outstanding sequence number
            ctx->outstanding_sequences.insert(seq);
            ctx->packets_sent.fetch_add(1);
            ctx->bytes_sent.fetch_add(HEADER_SIZE + payload_size);
        }

        // Wait for ctx->epoll_fd events or timeout
        int n =
            epoll_wait(ctx->epoll_fd.get(), events.data(), ctx->sockets.size(), EPOLL_TIMEOUT_MS);
        if (n == -1) {
            if (errno == EINTR) {
                continue;  // retry on signal
            }
            throw socket_exception(std::format("epoll_wait failed: {}", std::strerror(errno)));
        }

        for (int i = 0; i < n; ++i) {
            if (!(events[i].events & EPOLLIN)) {
                continue;  // skip non-readable events
            }

            // epoll_event was registered with ev.data.fd = sock.get()
            int ev_fd = events[i].data.fd;
            auto it = std::find_if(ctx->sockets.begin(), ctx->sockets.end(),
                                   [ev_fd](const unique_fd& u) { return u.get() == ev_fd; });
            if (it == ctx->sockets.end()) {
                continue;  // unknown fd
            }
            unique_fd& sock = *it;

            // Process all available packets on this socket
            while (true) {
                sockaddr_storage recv_remote{};
                int recv_remote_len = 0;
                bool would_block = post_recv(sock, recv_buffer, recv_remote, recv_remote_len);
                if (would_block) {
                    // Socket would block; break to wait for epoll_fd events
                    break;
                }

                // Process received packet
                if (recv_buffer.size() >= HEADER_SIZE) {
                    packet_header header;
                    std::memcpy(&header, recv_buffer.data(), sizeof(header));
                    uint64_t seq = be64toh(header.sequence_number);
                    uint64_t timestamp_ns = be64toh(header.timestamp_ns);
                    uint64_t now_ns = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                    uint64_t rtt_ns = now_ns - timestamp_ns;

                    // Check if sequence number is outstanding
                    auto seq_it = ctx->outstanding_sequences.find(seq);
                    if (seq_it != ctx->outstanding_sequences.end()) {
                        // Valid echo response
                        ctx->outstanding_sequences.erase(seq_it);
                        ctx->packets_received.fetch_add(1);
                        ctx->bytes_received.fetch_add(HEADER_SIZE + payload_size);
                        ctx->total_rtt_ns.fetch_add(rtt_ns);
                        update_min(ctx->min_rtt_ns, rtt_ns);
                        update_max(ctx->max_rtt_ns, rtt_ns);

                        // Post RTT sample to per-worker TDigest
                        post_rtt(ctx->curren_rtt_tdigest, 1000, rtt_ns);
                    }
                }
            }
        }
    }

    // Count remaining outstanding as dropped (add to any already tracked as dropped)
    ctx->packets_dropped.fetch_add(ctx->outstanding_sequences.size());

    // Post to global for merging
    {
        std::lock_guard<std::mutex> lock(g_worker_contexts_mutex);
        g_worker_rtt_tdigests.push_back(std::move(ctx->curren_rtt_tdigest));
    }
    ctx->curren_rtt_tdigest = nullptr;

    if (g_verbose.load())
        std::osyncstream(std::cout) << std::format(
            "[CPU {}] Worker shutting down. Stats: sent={}, recv={}, "
            "dropped={}\n",
            ctx->processor_id, ctx->packets_sent.load(), ctx->packets_received.load(),
            ctx->packets_dropped.load());
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
        << "  --server, -s <host>       - Server hostname or IP (required)\n"
        << "  --port, -p <port>         - Server UDP port (required)\n"
        << "  --payload, -l <bytes>     - Payload size in bytes (default: 64)\n"
        << "  --cores, -c <n>           - Number of cores/workers to use (default: all)\n"
        << "  --duration, -d <seconds>  - Test duration in seconds (default: 10)\n"
        << "  --rate, -r <pps>          - Packets per second total across all workers (0 = "
           "unlimited)\n"
        << "  --recvbuf, -b <bytes>     - Socket receive buffer size in bytes (default: "
           "4194304 = 4MB)\n"
        << "  --sockets, -k <n>         - Number of sockets to create per worker (default: 1)\n"
        << "  --verbose, -v             - Enable verbose logging (default: minimal)\n"
        << "  --stats-file, -o <path>   - Write final run statistics as JSON to file\n"
        << "  --help, -h                - Show this help\n";
}

/**
 * @brief Program entry point.
 *
 * Parses command-line arguments, creates worker
 * contexts and threads, runs for the requested duration and prints
 * final statistics.
 */
int main(int argc, char* argv[]) try {
    // Use ArgParser for command-line parsing
    ArgParser parser;
    parser.add_option("verbose", 'v', "0", false);
    parser.add_option("server", 's', "", true);
    parser.add_option("port", 'p', "7", true);  // Note: The IANA-assigned port for echo is 7
    parser.add_option("payload", 'l', "64", true);
    parser.add_option("cores", 'c', "0", true);
    parser.add_option("duration", 'd', "10", true);
    parser.add_option("rate", 'r', "10000", true);
    parser.add_option("recvbuf", 'b', "4194304", true);
    parser.add_option("sockets", 'k', "16", true);
    parser.add_option("stats-file", 'o', "", true);
    parser.add_option("help", 'h', "0", false);

    parser.parse(argc, argv);

    if (parser.is_set("help")) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string server_str = parser.get("server");
    const std::string port_arg = parser.get("port");
    const std::string payload_str = parser.get("payload");
    const std::string cores_str = parser.get("cores");
    const std::string duration_str = parser.get("duration");
    const std::string rate_str = parser.get("rate");
    const std::string recvbuf_str = parser.get("recvbuf");
    const std::string sockets_str = parser.get("sockets");
    const std::string stats_file = parser.get("stats-file");
    const std::string verbose_str = parser.get("verbose");
    size_t payload_size = 0;
    int duration_sec = 0;
    if (!verbose_str.empty() && verbose_str != "0") {
        g_verbose.store(true);
    }

    if (server_str.empty() || port_arg.empty()) {
        std::cerr << "Server and port are required\n";
        parser.print_help(argv[0]);
        return 1;
    }

    const char* server_ip = server_str.c_str();
    char* endptr = nullptr;
    long port_l = std::strtol(port_arg.c_str(), &endptr, 10);
    if (endptr == port_arg.c_str() || port_l <= 0 || port_l > 65535) {
        throw std::invalid_argument("Invalid port number");
    }
    int port = static_cast<int>(port_l);

    payload_size = static_cast<size_t>(std::strtoul(payload_str.c_str(), nullptr, 10));
    if (payload_size == 0 || payload_size > MAX_PAYLOAD_SIZE) {
        throw std::invalid_argument("Invalid payload size");
    }

    uint32_t num_processors = get_processor_count();
    uint32_t num_workers = num_processors;
    duration_sec = static_cast<int>(std::strtol(duration_str.c_str(), nullptr, 10));
    if (!cores_str.empty()) {
        int requested = static_cast<int>(std::strtol(cores_str.c_str(), nullptr, 10));
        if (requested > 0 && static_cast<uint32_t>(requested) <= num_processors) {
            num_workers = static_cast<uint32_t>(requested);
        }
    }

    g_rate_limit = static_cast<uint64_t>(std::strtoull(rate_str.c_str(), nullptr, 10));
    int sockets_per_worker = static_cast<int>(std::strtol(sockets_str.c_str(), nullptr, 10));
    if (sockets_per_worker <= 0) sockets_per_worker = 1;

    // Parse receive buffer size for sockets (default 4MB)
    int recvbuf = 4194304;
    if (!recvbuf_str.empty()) {
        long v = std::strtol(recvbuf_str.c_str(), nullptr, 10);
        if (v > 0) recvbuf = static_cast<int>(v);
    }
    uint64_t per_worker_display = g_rate_limit == 0 ? 0 : (g_rate_limit / num_workers);

    std::cout << std::format("Scalable UDP Echo Client\n");
    std::cout << std::format("Server: {}:{}\n", server_ip, port);
    std::cout << std::format("Payload size: {} bytes\n", payload_size);
    std::cout << std::format("Available processors: {}\n", num_processors);
    std::cout << std::format("Using {} worker(s)\n", num_workers);
    std::cout << std::format("Duration: {} seconds\n", duration_sec);
    std::cout << std::format("Rate limit: {} packets/sec total ({} per worker)\n", g_rate_limit,
                             per_worker_display);

    // Resolve server name (supports hostnames and IP literals)
    sockaddr_storage server_addr_storage = {};
    int server_addr_len = 0;

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* res = nullptr;
    std::string service_str = std::to_string(port);
    using unique_addrinfo = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;
    int gai_err = getaddrinfo(server_ip, service_str.c_str(), &hints, &res);
    if (gai_err != 0 || res == nullptr) {
        throw std::runtime_error(std::format("getaddrinfo failed for {}:{} with error: {}",
                                             server_ip, port, gai_strerror(gai_err)));
    }
    unique_addrinfo res_guard(res, freeaddrinfo);

    // Prefer IPv6 result when available, otherwise prefer IPv4, otherwise take first result
    addrinfo* chosen = nullptr;
    // First pass: look for IPv6
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET6) {
            chosen = ai;
            break;
        }
        if (chosen == nullptr) chosen = ai;
    }
    // If no IPv6, try to find IPv4 explicitly (chosen may already be set to first result)
    if (chosen == nullptr || chosen->ai_family != AF_INET6) {
        for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET) {
                chosen = ai;
                break;
            }
        }
    }

    if (chosen == nullptr) {
        throw std::runtime_error(
            std::format("No suitable address found for {}:{}", server_ip, port));
    }

    // Copy resolved sockaddr into storage and set length
    std::memcpy(&server_addr_storage, chosen->ai_addr, chosen->ai_addrlen);
    server_addr_len = static_cast<int>(chosen->ai_addrlen);
    int server_family = chosen->ai_family;

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Start TDigest merge thread
    std::thread tdigest_thread(tdigest_merge_thread);

    // Create worker contexts
    std::vector<std::unique_ptr<client_worker_context>> workers;

    for (uint32_t i = 0; i < num_workers; ++i) {
        auto ctx = std::make_unique<client_worker_context>();
        ctx->processor_id = i;

        // Copy resolved server address into worker context
        std::memcpy(&ctx->server_addr, &server_addr_storage, static_cast<size_t>(server_addr_len));
        ctx->server_addr_len = server_addr_len;

        // Create multiple UDP sockets for this worker, each bound to its own ephemeral port
        for (int sidx = 0; sidx < sockets_per_worker; ++sidx) {
            auto sock = create_udp_socket(server_family);

            // Set socket CPU affinity
            set_socket_cpu_affinity(sock, static_cast<uint16_t>(i));

            ctx->sockets.emplace_back(std::move(sock));
        }

        // Increase socket buffer sizes and bind each socket to an ephemeral port
        for (auto& sock : ctx->sockets) {
            set_socket_option(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvbuf),
                              sizeof(recvbuf));
            int sndbuf = recvbuf;
            set_socket_option(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf),
                              sizeof(sndbuf));

            bind_socket(sock, 0, server_family);
        }

        if (g_verbose.load()) {
            for (const auto& sock : ctx->sockets) {
                auto [addr, len] = get_socket_name(sock);
                if (addr.ss_family == AF_INET) {
                    sockaddr_in* in_addr = reinterpret_cast<sockaddr_in*>(&addr);
                    char ip_str[INET_ADDRSTRLEN] = {};
                    if (inet_ntop(AF_INET, &in_addr->sin_addr, ip_str, sizeof(ip_str)) == nullptr) {
                        std::perror("inet_ntop(AF_INET)");
                        std::strcpy(ip_str, "?");
                    }
                    std::cout << std::format("Socket on CPU {} bound to {}:{}\n", i, ip_str,
                                             ntohs(in_addr->sin_port));
                } else if (addr.ss_family == AF_INET6) {
                    sockaddr_in6* in6_addr = reinterpret_cast<sockaddr_in6*>(&addr);
                    char ip_str[INET6_ADDRSTRLEN] = {};
                    if (inet_ntop(AF_INET6, &in6_addr->sin6_addr, ip_str, sizeof(ip_str)) ==
                        nullptr) {
                        std::perror("inet_ntop(AF_INET6)");
                        std::strcpy(ip_str, "?");
                    }
                    std::cout << std::format("Socket on CPU {} bound to [{}]:{}\n", i, ip_str,
                                             ntohs(in6_addr->sin6_port));
                }
            }
        }

        // Create epoll_fd for the worker and associate each socket with it
        ctx->epoll_fd = create_epoll();

        for (auto& s : ctx->sockets) {
            associate_socket_with_epoll(s, ctx->epoll_fd, static_cast<uintptr_t>(s.get()));
        }

        if (g_verbose.load())
            std::cout << std::format("Created socket and epoll_fd for CPU {}\n", i);
        workers.push_back(std::move(ctx));
    }

    if (workers.empty()) {
        std::cerr << "Failed to create any workers\n";
        return 1;
    }

    // Compute per-worker rate: divide global total equally among workers
    uint64_t per_worker_rate = 0;
    if (g_rate_limit == 0) {
        per_worker_rate = 0;  // 0 == unlimited
    } else {
        per_worker_rate = g_rate_limit / static_cast<uint64_t>(workers.size());
    }
    for (const auto& ctx : workers) {
        ctx->per_worker_rate = per_worker_rate;
    }

    // Start worker threads
    for (auto& ctx : workers) {
        ctx->worker_thread = std::thread(worker_thread_func, ctx.get(), payload_size);
    }

    if (g_verbose.load())
        std::cout << std::format("\nClient running for {} seconds. Press Ctrl+C to stop early.\n\n",
                                 duration_sec);

    // Run for specified duration
    auto start_time = std::chrono::steady_clock::now();
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= duration_sec) {
            break;
        }

        // Print interim stats
        uint64_t total_sent = 0, total_recv = 0;
        for (const auto& ctx : workers) {
            total_sent += ctx->packets_sent.load();
            total_recv += ctx->packets_received.load();
        }
        std::cout << std::format("Progress: sent={}, recv={}, in-flight={}\n", total_sent,
                                 total_recv, total_sent - total_recv);
    }

    g_shutdown.store(true);
    std::cout << "\nStopping workers...\n";

    // Close epoll_fds to wake up worker threads
    for (const auto& ctx : workers) {
        if (ctx->epoll_fd) {
            ctx->epoll_fd.reset();
        }
    }

    // Wait for worker threads
    for (const auto& ctx : workers) {
        if (ctx->worker_thread.joinable()) {
            ctx->worker_thread.join();
        }
    }

    // Close sockets
    for (const auto& ctx : workers) {
        ctx->sockets.clear();
    }

    tdigest_thread.join();

    merge_tdigest();

    // Calculate and print final stats
    uint64_t total_sent = 0, total_recv = 0, total_dropped = 0;
    uint64_t total_bytes_sent = 0, total_bytes_recv = 0;
    uint64_t total_rtt = 0;
    uint64_t min_rtt = UINT64_MAX, max_rtt = 0;

    for (const auto& ctx : workers) {
        total_sent += ctx->packets_sent.load();
        total_recv += ctx->packets_received.load();
        total_dropped += ctx->packets_dropped.load();
        total_bytes_sent += ctx->bytes_sent.load();
        total_bytes_recv += ctx->bytes_received.load();
        total_rtt += ctx->total_rtt_ns.load();

        uint64_t worker_min = ctx->min_rtt_ns.load();
        uint64_t worker_max = ctx->max_rtt_ns.load();
        if (worker_min < min_rtt) min_rtt = worker_min;
        if (worker_max > max_rtt) max_rtt = worker_max;
    }

    // Convert RTT aggregates from nanoseconds into milliseconds for display
    double avg_rtt_ms =
        total_recv > 0 ? (static_cast<double>(total_rtt) / total_recv / 1'000'000.0) : 0.0;
    double min_rtt_ms = min_rtt != UINT64_MAX ? static_cast<double>(min_rtt) / 1'000'000.0 : 0.0;
    double max_rtt_ms = static_cast<double>(max_rtt) / 1'000'000.0;

    auto actual_duration = std::chrono::steady_clock::now() - start_time;
    double duration_s =
        std::chrono::duration_cast<std::chrono::milliseconds>(actual_duration).count() / 1000.0;
    double pps_sent = total_sent / duration_s;
    double pps_recv = total_recv / duration_s;
    double mbps_sent = (total_bytes_sent * 8.0) / (duration_s * 1000000.0);
    double mbps_recv = (total_bytes_recv * 8.0) / (duration_s * 1000000.0);

    std::cout << std::format("\n===== Final Statistics =====\n");
    std::cout << std::format("Duration: {:.2f} seconds\n", duration_s);
    std::cout << std::format("Packets sent: {} ({:.0f} pps)\n", total_sent, pps_sent);
    std::cout << std::format("Packets received: {} ({:.0f} pps)\n", total_recv, pps_recv);
    std::cout << std::format("Packets dropped: {} ({:.2f}%)\n", total_dropped,
                             total_sent > 0 ? (100.0 * total_dropped / total_sent) : 0.0);
    std::cout << std::format("Bytes sent: {} ({:.2f} Mbps)\n", total_bytes_sent, mbps_sent);
    std::cout << std::format("Bytes received: {} ({:.2f} Mbps)\n", total_bytes_recv, mbps_recv);
    std::cout << std::format("RTT (min/avg/max): {:.2f}/{:.2f}/{:.2f} ms\n", min_rtt_ms, avg_rtt_ms,
                             max_rtt_ms);

    // TDigest stores RTT samples in milliseconds, so percentiles are in ms as well.
    std::cout << std::format(
        "RTT Percentiles (ms): p50={:.2f} p90={:.2f} p99={:.2f} p99.9={:.2f}\n",
        g_overall_rtt_tdigest.percentile(0.50), g_overall_rtt_tdigest.percentile(0.90),
        g_overall_rtt_tdigest.percentile(0.99), g_overall_rtt_tdigest.percentile(0.999));

    // Optionally write final statistics to a file as JSON if requested
    if (!stats_file.empty()) {
        std::ofstream ofs(stats_file, std::ios::out | std::ios::trunc);
        if (!ofs) {
            std::cerr << std::format("Failed to open stats file '{}' for writing\n", stats_file);
        } else {
            ofs << std::fixed << std::setprecision(2);
            // Build simple JSON object
            double drop_pct = total_sent > 0 ? (100.0 * total_dropped / total_sent) : 0.0;
            ofs << "{\n";
            ofs << std::format("  \"duration_s\": {:.2f},\n", duration_s);
            ofs << std::format("  \"packets_sent\": {},\n", total_sent);
            ofs << std::format("  \"packets_received\": {},\n", total_recv);
            ofs << std::format("  \"packets_dropped\": {},\n", total_dropped);
            ofs << std::format("  \"packets_dropped_pct\": {:.2f},\n", drop_pct);
            ofs << std::format("  \"pps_sent\": {:.2f},\n", pps_sent);
            ofs << std::format("  \"pps_recv\": {:.2f},\n", pps_recv);
            ofs << std::format("  \"bytes_sent\": {},\n", total_bytes_sent);
            ofs << std::format("  \"bytes_received\": {},\n", total_bytes_recv);
            ofs << std::format("  \"mbps_sent\": {:.2f},\n", mbps_sent);
            ofs << std::format("  \"mbps_recv\": {:.2f},\n", mbps_recv);
            ofs << std::format("  \"rtt_min_ms\": {:.2f},\n", min_rtt_ms);
            ofs << std::format("  \"rtt_avg_ms\": {:.2f},\n", avg_rtt_ms);
            ofs << std::format("  \"rtt_max_ms\": {:.2f},\n", max_rtt_ms);
            ofs << std::format("  \"rtt_p50_ms\": {:.2f},\n",
                               g_overall_rtt_tdigest.percentile(0.50));
            ofs << std::format("  \"rtt_p90_ms\": {:.2f},\n",
                               g_overall_rtt_tdigest.percentile(0.90));
            ofs << std::format("  \"rtt_p99_ms\": {:.2f},\n",
                               g_overall_rtt_tdigest.percentile(0.99));
            ofs << std::format("  \"rtt_p999_ms\": {:.2f}\n",
                               g_overall_rtt_tdigest.percentile(0.999));
            ofs << "}\n";
            ofs.close();
            if (g_verbose.load()) std::cout << std::format("Wrote JSON stats to {}\n", stats_file);
        }
    }

    return 0;
} catch (const socket_exception& ex) {
    std::cerr << "Socket error: " << ex.what() << "\n";
    return 1;
} catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    return 1;
} catch (...) {
    std::cerr << "Unknown error occurred\n";
    return 1;
}