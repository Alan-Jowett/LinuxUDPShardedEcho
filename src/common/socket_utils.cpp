#include "socket_utils.hpp"

#include <sys/epoll.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>

unique_fd create_udp_socket(int family) {
	int fd = ::socket(family, SOCK_DGRAM, 0);
	if (fd == -1) {
		throw socket_exception(std::format("socket() failed: {}", std::strerror(errno)));
	}
	// set non-blocking
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1) flags = 0;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		int e = errno;
		::close(fd);
		throw socket_exception(std::format("fcntl(O_NONBLOCK) failed: {}", std::strerror(e)));
	}
	return unique_fd(fd);
}

void set_socket_cpu_affinity(const unique_fd& sock, uint16_t processor_id) {
	// Not portable on Linux for sockets; noop or set SO_INCOMING_CPU if available.
	(void)sock;
	(void)processor_id;
	// Set SO_REUSEADDR and SO_REUSEPORT to allow multiple sockets on same port
	int opt = 1;
	if (::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
		throw socket_exception(std::format("setsockopt(SO_REUSEADDR) failed: {}", std::strerror(errno)));
	}
#ifdef SO_REUSEPORT
	if (::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
		throw socket_exception(std::format("setsockopt(SO_REUSEPORT) failed: {}", std::strerror(errno)));
	}
#endif
}

unique_fd create_epoll() {
	int efd = ::epoll_create1(0);
	if (efd == -1) {
		throw socket_exception(std::format("epoll_create1 failed: {}", std::strerror(errno)));
	}
	return unique_fd(efd);
}

unique_fd create_epoll_and_associate(const unique_fd& sock) {
	unique_fd ep = create_epoll();
	associate_socket_with_epoll(sock, ep, 0);
	return ep;
}

void associate_socket_with_epoll(const unique_fd& sock, unique_fd& epoll,
								uintptr_t /*completion_key*/) {
	struct epoll_event ev;
	std::memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = sock.get();
	// Avoid storing the address of the stack parameter `sock` in the event
	// data (that would be a pointer to a local variable). Use the fd in
	// `ev.data.fd` as the identifier. Do not overwrite the union by
	// assigning to `data.ptr` after setting `data.fd`.
	if (getenv("ECHO_VERBOSE")) {
		std::cerr << std::format("associate_socket_with_epoll: epoll={} sock={}\n",
								 epoll.get(), sock.get());
	}
	if (::epoll_ctl(epoll.get(), EPOLL_CTL_ADD, sock.get(), &ev) == -1) {
		throw socket_exception(std::format("epoll_ctl ADD failed: {}", std::strerror(errno)));
	}
}

void set_thread_affinity(uint32_t processor_id) {
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(processor_id, &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
		throw socket_exception(std::format("pthread_setaffinity_np failed: {}", std::strerror(errno)));
	}
}

uint32_t get_processor_count() {
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n < 1) return 1u;
	return static_cast<uint32_t>(n);
}

void bind_socket(const unique_fd& sock, uint16_t port, int family) {
	if (!sock.valid()) throw socket_exception("bind_socket: invalid socket");
	if (family == AF_INET) {
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(port);
		if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
			throw socket_exception(std::format("bind(AF_INET) failed: {}", std::strerror(errno)));
		}
	} else if (family == AF_INET6) {
		sockaddr_in6 addr{};
		addr.sin6_family = AF_INET6;
		addr.sin6_addr = in6addr_any;
		addr.sin6_port = htons(port);
		if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
			throw socket_exception(std::format("bind(AF_INET6) failed: {}", std::strerror(errno)));
		}
	} else {
		throw socket_exception("bind_socket: unsupported family");
	}
}

void set_socket_option(const unique_fd& sock, int level, int optname, const char* optval,
					   int optlen) {
	if (!sock.valid()) throw socket_exception("set_socket_option: invalid socket");
	if (::setsockopt(sock.get(), level, optname, optval, optlen) == -1) {
		throw socket_exception(std::format("setsockopt failed: {}", std::strerror(errno)));
	}
}

bool post_recv(const unique_fd& sock, std::vector<uint8_t>& buffer,
			   sockaddr_storage& out_remote_addr, int& out_remote_addr_len) {
	// For epoll edge-triggered non-blocking sockets we perform a single non-blocking
	// recvfrom into ctx->buffer. Return true if the caller should wait on epoll for
	// readability (i.e. the operation would block), false if recv completed immediately.
	if (!sock.valid()) throw socket_exception("post_recv: invalid socket");
	sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);
	ssize_t n;
	while (true) {
		// Ensure buffer has space for max packet
		if (buffer.size() < MAX_PACKET_SIZE) buffer.resize(MAX_PACKET_SIZE);
		n = ::recvfrom(sock.get(), buffer.data(), MAX_PACKET_SIZE, 0,
					   reinterpret_cast<sockaddr*>(&addr), &addrlen);
		if (n == -1) {
			if (errno == EINTR) {
				continue; // retry on signal
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				// Would block: caller should wait for epoll readability before retrying
				return true;
			}
			throw socket_exception(std::format("recvfrom failed: {}", std::strerror(errno)));
		}
		break;
	}
	// store remote addr
	out_remote_addr_len = static_cast<int>(addrlen);
	std::memcpy(&out_remote_addr, &addr, std::min(sizeof(out_remote_addr), (size_t)addrlen));
	// Resize buffer to actual received size
	buffer.resize(static_cast<size_t>(n));
	// Copy received data into buffer (we read directly into buffer so already populated)
	// Note: recvfrom wrote into buffer.data(), so nothing to copy here.
	(void)n; // n is the number of bytes received
	return false; // completed immediately, no need to wait on epoll
}

bool post_send(const unique_fd& sock, std::vector<uint8_t>& buffer, size_t len,
			   const sockaddr* dest_addr, int dest_addr_len) {
	// Returns true if the caller should wait on epoll for writability (EAGAIN/EWOULDBLOCK).
	if (!sock.valid()) throw socket_exception("post_send: invalid socket");
	// `len` is the payload length (excluding our HEADER_SIZE prefix).
	if (HEADER_SIZE + len > MAX_PACKET_SIZE) {
		throw socket_exception("post_send: data too large");
	}
	// Ensure buffer has space for header + payload
	if (buffer.size() < HEADER_SIZE + len) buffer.resize(HEADER_SIZE + len);
	ssize_t n;
	while (true) {
		n = ::sendto(sock.get(), buffer.data(), static_cast<size_t>(len) + HEADER_SIZE, 0,
			     dest_addr, dest_addr_len);
		if (n == -1) {
			if (errno == EINTR) {
				continue; // retry
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				// Would block: caller should wait for epoll writability before retrying
				return true;
			}
			throw socket_exception(std::format("sendto failed: {}", std::strerror(errno)));
		}
		break;
	}
	(void)n; // number of bytes sent
	return false; // completed immediately
}

int send_sync(const unique_fd& sock, const char* data, size_t len, const sockaddr* dest_addr,
			  int dest_addr_len) {
	if (!sock.valid()) throw socket_exception("send_sync: invalid socket");
	ssize_t n = ::sendto(sock.get(), data, len, 0, dest_addr, dest_addr_len);
	if (n == -1) {
		throw socket_exception(std::format("sendto failed: {}", std::strerror(errno)));
	}
	return static_cast<int>(n);
}

uint64_t get_timestamp_ns() {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
		throw socket_exception(std::format("clock_gettime failed: {}", std::strerror(errno)));
	}
	return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

std::pair<sockaddr_storage, int> get_socket_name(const unique_fd& sock) {
	if (!sock.valid()) throw socket_exception("get_socket_name: invalid socket");
	sockaddr_storage addr{};
	socklen_t len = sizeof(addr);
	if (::getsockname(sock.get(), reinterpret_cast<sockaddr*>(&addr), &len) == -1) {
		throw socket_exception(std::format("getsockname failed: {}", std::strerror(errno)));
	}
	return {addr, static_cast<int>(len)};
}

std::string get_last_error_message() {
	return std::strerror(errno);
}

