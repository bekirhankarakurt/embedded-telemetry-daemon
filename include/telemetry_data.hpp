/**
 * @file telemetry_data.hpp
 * @brief Telemetry sample model, JSON-lines serialization and binary wire format.
 *
 * Binary packet layout (all multi-byte fields in network byte order / big-endian):
 *
 *   [ PacketHeader (24 B) ][ payload (168 B) ][ CRC-32 (4 B) ]
 *
 * The CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) covers header + payload.
 * Floating point values are transmitted as their IEEE-754 binary64 bit pattern.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace etd {

static_assert(std::numeric_limits<double>::is_iec559, "IEEE-754 doubles required for the wire format");
static_assert(sizeof(double) == sizeof(std::uint64_t), "double must be 64 bits");

/// One snapshot of system state produced by the collector.
struct TelemetrySample {
    std::uint64_t seq{0};      ///< Monotonically increasing sequence number.
    std::uint64_t mono_ns{0};  ///< CLOCK_MONOTONIC timestamp [ns].
    std::uint64_t wall_ns{0};  ///< CLOCK_REALTIME timestamp [ns since Unix epoch].

    double cpu_percent{0.0};  ///< Aggregate CPU utilisation over the last period [0..100].

    std::uint64_t mem_total_kb{0};      ///< MemTotal [KiB].
    std::uint64_t mem_available_kb{0};  ///< MemAvailable [KiB].
    std::uint64_t mem_used_kb{0};       ///< MemTotal - MemAvailable [KiB].
    std::uint64_t swap_total_kb{0};     ///< SwapTotal [KiB].
    std::uint64_t swap_free_kb{0};      ///< SwapFree [KiB].

    std::uint64_t uptime_s{0};  ///< System uptime [s].
    double load1{0.0};          ///< 1-minute load average.
    double load5{0.0};          ///< 5-minute load average.
    double load15{0.0};         ///< 15-minute load average.

    std::uint64_t net_rx_bytes{0};  ///< Sum over non-loopback interfaces.
    std::uint64_t net_tx_bytes{0};
    std::uint64_t net_rx_packets{0};
    std::uint64_t net_tx_packets{0};
    std::uint64_t net_rx_errors{0};
    std::uint64_t net_tx_errors{0};
    std::uint64_t net_rx_drops{0};
    std::uint64_t net_tx_drops{0};
    double net_rx_bytes_per_s{0.0};  ///< Receive throughput over the last period.
    double net_tx_bytes_per_s{0.0};  ///< Transmit throughput over the last period.

    friend bool operator==(const TelemetrySample& a, const TelemetrySample& b) noexcept;
    friend bool operator!=(const TelemetrySample& a, const TelemetrySample& b) noexcept { return !(a == b); }
};

/// Wire-format constants.
namespace wire {
inline constexpr std::uint32_t kMagic = 0x544C4D59U;  ///< ASCII "TLMY".
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kPayloadSize = std::size_t{21} * 8;
inline constexpr std::size_t kCrcSize = 4;
inline constexpr std::size_t kPacketSize = kHeaderSize + kPayloadSize + kCrcSize;
static_assert(kPacketSize == 196, "wire format size changed; bump kVersion");
static_assert(kPayloadSize <= std::numeric_limits<std::uint16_t>::max(), "payload length must fit u16");
}  // namespace wire

/// Message type carried in the packet header.
enum class MessageType : std::uint8_t {
    Telemetry = 1,
};

/// Decoded binary packet header (host byte order).
struct PacketHeader {
    std::uint32_t magic{wire::kMagic};
    std::uint8_t version{wire::kVersion};
    MessageType type{MessageType::Telemetry};
    std::uint16_t payload_len{static_cast<std::uint16_t>(wire::kPayloadSize)};
    std::uint64_t seq{0};
    std::uint64_t timestamp_ns{0};  ///< Monotonic timestamp of the sample.
};

/// Reasons a binary packet can be rejected.
enum class DecodeError : std::uint8_t {
    None,
    TooShort,
    BadMagic,
    BadVersion,
    BadType,
    BadLength,
    BadCrc,
};

/// Human-readable name of a decode error.
[[nodiscard]] std::string_view to_string(DecodeError e) noexcept;

/// Result of decoding a binary packet.
struct DecodeResult {
    DecodeError error{DecodeError::None};
    TelemetrySample sample{};
    [[nodiscard]] bool ok() const noexcept { return error == DecodeError::None; }
};

/// Fixed-size binary packet buffer.
using Packet = std::array<std::uint8_t, wire::kPacketSize>;

/// Compute CRC-32 (IEEE 802.3) over @p len bytes; @p seed allows incremental use.
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t seed = 0) noexcept;

/// Serialize a sample into the binary wire format.
[[nodiscard]] Packet encode_binary(const TelemetrySample& s) noexcept;

/// Validate and decode a binary packet (magic, version, type, length, CRC).
[[nodiscard]] DecodeResult decode_binary(const std::uint8_t* data, std::size_t len) noexcept;

/// Serialize a sample as a single-line JSON object terminated by '\n'.
[[nodiscard]] std::string to_json_line(const TelemetrySample& s);

/// Parse a JSON line produced by to_json_line(). Returns std::nullopt on malformed input.
[[nodiscard]] std::optional<TelemetrySample> parse_json_line(std::string_view line);

}  // namespace etd
