/**
 * @file udp_sender.hpp
 * @brief RAII, non-blocking IPv4 UDP sender (unicast or broadcast).
 */
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace etd {

/// Destination configuration.
struct UdpConfig {
    std::string host{"127.0.0.1"};  ///< Dotted-quad IPv4 address.
    std::uint16_t port{5000};       ///< Destination port (non-zero).
    bool broadcast{false};          ///< Enable SO_BROADCAST.
};

/// Outcome of a single send.
enum class SendResult : std::uint8_t {
    Sent,     ///< Datagram handed to the kernel.
    Dropped,  ///< Transient back-pressure (EAGAIN/ENOBUFS); datagram discarded.
    Error,    ///< Other socket error (e.g. ENETUNREACH, ECONNREFUSED).
};

/// Parse and validate a destination. Returns an error message, or std::nullopt on success.
[[nodiscard]] std::optional<std::string> validate_udp_config(const UdpConfig& cfg, sockaddr_in* out = nullptr);

/**
 * Owns a non-blocking UDP socket. Movable, non-copyable; the descriptor is
 * closed in the destructor. send() never blocks: back-pressure is counted as a
 * drop so the real-time sampling path cannot stall on the network.
 * Counters are atomics and may be read from any thread.
 */
class UdpSender {
public:
    /// @throws std::invalid_argument on a bad address, std::system_error on socket failure.
    explicit UdpSender(const UdpConfig& cfg);
    ~UdpSender();

    UdpSender(UdpSender&& other) noexcept;
    UdpSender& operator=(UdpSender&& other) noexcept;
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    /// Send one datagram. Retries on EINTR. Not thread-safe against concurrent move.
    SendResult send(const void* data, std::size_t len) noexcept;

    [[nodiscard]] std::uint64_t sent() const noexcept { return sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }
    /// errno of the most recent Error result (0 if none).
    [[nodiscard]] int last_errno() const noexcept { return last_errno_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    void close_fd() noexcept;

    int fd_{-1};
    sockaddr_in dest_{};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<int> last_errno_{0};
};

}  // namespace etd
