#include "udp_sender.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace etd {

std::optional<std::string> validate_udp_config(const UdpConfig& cfg, sockaddr_in* out) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (::inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1) {
        return "invalid IPv4 address '" + cfg.host + "' (expected dotted quad, e.g. 192.168.1.10)";
    }
    if (cfg.port == 0) {
        return std::string("destination port must be in 1..65535");
    }
    const std::uint32_t host_order = ntohl(addr.sin_addr.s_addr);
    if (host_order == 0U) {
        return std::string("destination 0.0.0.0 is not a valid target");
    }
    if (host_order == 0xFFFFFFFFU && !cfg.broadcast) {
        return std::string("255.255.255.255 requires --broadcast");
    }
    addr.sin_port = htons(cfg.port);
    if (out != nullptr) {
        *out = addr;
    }
    return std::nullopt;
}

UdpSender::UdpSender(const UdpConfig& cfg) {
    if (auto err = validate_udp_config(cfg, &dest_)) {
        throw std::invalid_argument(*err);
    }
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "socket(AF_INET, SOCK_DGRAM)");
    }
    if (cfg.broadcast) {
        const int on = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) != 0) {
            const int e = errno;
            close_fd();
            throw std::system_error(e, std::generic_category(), "setsockopt(SO_BROADCAST)");
        }
    }
}

UdpSender::~UdpSender() {
    close_fd();
}

UdpSender::UdpSender(UdpSender&& other) noexcept
    : fd_(other.fd_),
      dest_(other.dest_),
      sent_(other.sent_.load()),
      dropped_(other.dropped_.load()),
      errors_(other.errors_.load()),
      last_errno_(other.last_errno_.load()) {
    other.fd_ = -1;
}

UdpSender& UdpSender::operator=(UdpSender&& other) noexcept {
    if (this != &other) {
        close_fd();
        fd_ = other.fd_;
        dest_ = other.dest_;
        sent_.store(other.sent_.load());
        dropped_.store(other.dropped_.load());
        errors_.store(other.errors_.load());
        last_errno_.store(other.last_errno_.load());
        other.fd_ = -1;
    }
    return *this;
}

void UdpSender::close_fd() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

SendResult UdpSender::send(const void* data, std::size_t len) noexcept {
    if (fd_ < 0) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        last_errno_.store(EBADF, std::memory_order_relaxed);
        return SendResult::Error;
    }
    for (;;) {
        const ssize_t n =
            ::sendto(fd_, data, len, MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&dest_), sizeof dest_);
        if (n >= 0) {
            sent_.fetch_add(1, std::memory_order_relaxed);
            return SendResult::Sent;
        }
        const int e = errno;
        if (e == EINTR) {
            continue;
        }
        if (e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return SendResult::Dropped;
        }
        errors_.fetch_add(1, std::memory_order_relaxed);
        last_errno_.store(e, std::memory_order_relaxed);
        return SendResult::Error;
    }
}

}  // namespace etd
