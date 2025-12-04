/**
 * @file socket_utils.hpp
 * @brief Cross-cutting Win32 socket utilities and epoll helpers used by the demo.
 *
 * This header provides small helpers and types for creating UDP sockets,
 * posting overlapped operations and interacting with IO Completion Ports
 * (epoll) on Windows. It also defines packet framing constants used by the
 * echo server/client.
 *
 * @copyright Copyright (c) 2025 LinuxUDPShardedEcho Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <arpa/inet.h>   // inet_pton / inet_ntop
#include <errno.h>       // errno
#include <fcntl.h>       // fcntl
#include <netdb.h>       // getaddrinfo / freeaddrinfo
#include <netinet/in.h>  // sockaddr_in / sockaddr_in6, htons/htonl
#include <sys/socket.h>  // AF_INET, AF_INET6, socket, sockaddr, etc.
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Packet header for tracking sequence numbers
#pragma pack(push, 1)
/**
 * @brief Packet framing header prepended to each UDP payload.
 *
 * The header includes a 64-bit sequence number and a 64-bit timestamp in
 * nanoseconds. It is packed to avoid padding between fields.
 */
struct packet_header {
    /// Monotonic sequence number assigned by sender.
    uint64_t sequence_number;
    /// Sender timestamp in nanoseconds when the packet was created.
    uint64_t timestamp_ns;
};
#pragma pack(pop)

/// Maximum UDP payload size (practical limit for IPv4/IPv6 datagrams).
constexpr size_t MAX_PACKET_SIZE = 65507;  // Max UDP payload size
/// Size of the packet header defined above.
constexpr size_t HEADER_SIZE = sizeof(packet_header);
/// Maximum application payload size after subtracting the header.
constexpr size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - HEADER_SIZE;

// Shared configuration constants
/// Number of simultaneous outstanding asynchronous I/O operations per socket.
constexpr size_t OUTSTANDING_OPS = 16;  // Number of outstanding I/O operations per socket
/// Timeout in milliseconds used when polling an epoll for events.
constexpr uint32_t EPOLL_TIMEOUT_MS = 10;  // EPOLL polling timeout in milliseconds
/// Timeout used specifically during shutdown checks on the EPOLL.
constexpr uint32_t EPOLL_SHUTDOWN_TIMEOUT_MS = 1000;  // EPOLL timeout for shutdown check

class unique_fd {
   public:
    explicit unique_fd(int fd = -1) : fd_(fd) {}
    ~unique_fd() {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    // Disable copy
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    // Enable move
    unique_fd(unique_fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            if (fd_ != -1) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ != -1; }

    // Make this convertible to bool
    explicit operator bool() const { return valid(); }

    int reset(int new_fd = -1) {
        if (fd_ != -1) {
            ::close(fd_);
        }
        fd_ = new_fd;
        return fd_;
    }

   private:
    int fd_;
};

struct socket_exception : public std::exception {
    explicit socket_exception(const std::string& message) : message_(message) {}
    const char* what() const noexcept override { return message_.c_str(); }

   private:
    std::string message_;
};

/**
 * @brief Create a UDP socket for the specified address family.
 *
 * @param family Address family (AF_INET or AF_INET6). Defaults to AF_INET.
 * @return A RAII `unique_fd` owning the created SOCKET.
 */
unique_fd create_udp_socket(int family = AF_INET);

/**
 * @brief Set the CPU affinity for a socket (Windows SIO_CPU_AFFINITY).
 *
 * @param sock The socket to configure.
 * @param processor_id Logical processor index to bind the socket to.
 */
void set_socket_cpu_affinity(const unique_fd& sock, uint16_t processor_id);

/**
 * @brief Create an unassociated IO Completion Port (EPOLL).
 * @return A `unique_fd` handle for the created EPOLL.
 */
unique_fd create_epoll();

/**
 * @brief Create an epoll and associate a socket with it.
 *
 * Convenience helper that creates an epoll and associates `sock` so the
 * application can begin posting asynchronous operations and receive
 * completions via the epoll.
 */
unique_fd create_epoll_and_associate(const unique_fd& sock);

/**
 * @brief Associate an existing socket with an existing epoll.
 *
 * @param sock Socket to associate.
 * @param epoll epoll handle to associate with.
 * @param completion_key Completion key (typically used to identify socket/thread).
 */
void associate_socket_with_epoll(const unique_fd& sock, const unique_fd& epoll,
                                 uintptr_t completion_key);

/**
 * @brief Set the current thread's processor affinity.
 *
 * This is used by worker threads that should be pinned to a specific CPU.
 */
void set_thread_affinity(uint32_t processor_id);

/**
 * @brief Query the number of logical processors available on the system.
 */
uint32_t get_processor_count();

/**
 * @brief Bind a UDP socket to the given port and address family.
 *
 * Throws `socket_exception` on failure.
 */
void bind_socket(const unique_fd& sock, uint16_t port, int family = AF_INET);

/**
 * @brief Helper around `setsockopt` that throws `socket_exception` on error.
 */
void set_socket_option(const unique_fd& sock, int level, int optname, const char* optval,
                       int optlen);

/**
 * @brief Post an asynchronous receive (WSARecvFrom) using the provided context.
 */
// Returns true if the caller should wait on epoll for the socket to become readable
// (i.e. the operation would block and should be retried when epoll reports readability).
bool post_recv(const unique_fd& sock, std::vector<uint8_t>& buffer,
               sockaddr_storage& out_remote_addr, int& out_remote_addr_len);

/**
 * @brief Post an asynchronous send (WSASendTo) using the provided context.
 *
 * The `data` pointer is copied into the `ctx->buffer` prior to posting.
 */
// Returns true if the caller should wait on epoll for the socket to become writable
// (i.e. the operation would block and should be retried when epoll reports writability).
bool post_send(const unique_fd& sock, std::vector<uint8_t>& buffer, size_t len,
               const sockaddr* dest_addr, int dest_addr_len);

/**
 * @brief Synchronously send a UDP datagram using `sendto`.
 *
 * Returns number of bytes sent on success, or throws socket_exception on error.
 */
int send_sync(const unique_fd& sock, const char* data, size_t len, const sockaddr* dest_addr,
              int dest_addr_len);

/**
 * @brief Return a monotonic timestamp in nanoseconds.
 */
uint64_t get_timestamp_ns();

/**
 * @brief Return the local socket address (sockname) for a socket.
 *
 * @return Pair of `sockaddr_storage` and length; throws on error.
 */
std::pair<sockaddr_storage, int> get_socket_name(const unique_fd& sock);

/**
 * @brief Format the last Win32 socket error into a human-readable string.
 */
std::string get_last_error_message();
