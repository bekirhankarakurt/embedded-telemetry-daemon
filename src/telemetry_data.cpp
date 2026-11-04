#include "telemetry_data.hpp"

#include <bitset>
#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace etd {
namespace {

/// Describes one numeric field; exactly one of the member pointers is set.
struct Field {
    const char* name;
    std::uint64_t TelemetrySample::*u64;
    double TelemetrySample::*f64;
    bool in_payload;  ///< false for fields carried in the binary header (seq, mono_ns).
};

using S = TelemetrySample;

// Order defines both the JSON key order and the binary payload layout.
constexpr std::array<Field, 23> kFields{{
    {"seq", &S::seq, nullptr, false},
    {"mono_ns", &S::mono_ns, nullptr, false},
    {"wall_ns", &S::wall_ns, nullptr, true},
    {"cpu_pct", nullptr, &S::cpu_percent, true},
    {"mem_total_kb", &S::mem_total_kb, nullptr, true},
    {"mem_avail_kb", &S::mem_available_kb, nullptr, true},
    {"mem_used_kb", &S::mem_used_kb, nullptr, true},
    {"swap_total_kb", &S::swap_total_kb, nullptr, true},
    {"swap_free_kb", &S::swap_free_kb, nullptr, true},
    {"uptime_s", &S::uptime_s, nullptr, true},
    {"load1", nullptr, &S::load1, true},
    {"load5", nullptr, &S::load5, true},
    {"load15", nullptr, &S::load15, true},
    {"net_rx_bytes", &S::net_rx_bytes, nullptr, true},
    {"net_tx_bytes", &S::net_tx_bytes, nullptr, true},
    {"net_rx_packets", &S::net_rx_packets, nullptr, true},
    {"net_tx_packets", &S::net_tx_packets, nullptr, true},
    {"net_rx_errors", &S::net_rx_errors, nullptr, true},
    {"net_tx_errors", &S::net_tx_errors, nullptr, true},
    {"net_rx_drops", &S::net_rx_drops, nullptr, true},
    {"net_tx_drops", &S::net_tx_drops, nullptr, true},
    {"net_rx_Bps", nullptr, &S::net_rx_bytes_per_s, true},
    {"net_tx_Bps", nullptr, &S::net_tx_bytes_per_s, true},
}};

constexpr std::size_t count_payload_fields() noexcept {
    std::size_t n = 0;
    for (const auto& f : kFields) {
        n += f.in_payload ? 1U : 0U;
    }
    return n;
}
static_assert(count_payload_fields() * 8 == wire::kPayloadSize, "payload table and kPayloadSize disagree");

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
        }
        table[i] = c;
    }
    return table;
}
constexpr auto kCrcTable = make_crc_table();

// --- Big-endian helpers (byte-wise, independent of host endianness) -----------

void put_u8(std::uint8_t*& p, std::uint8_t v) noexcept {
    *p++ = v;
}

void put_u16(std::uint8_t*& p, std::uint16_t v) noexcept {
    *p++ = static_cast<std::uint8_t>(v >> 8U);
    *p++ = static_cast<std::uint8_t>(v);
}

void put_u32(std::uint8_t*& p, std::uint32_t v) noexcept {
    for (int shift = 24; shift >= 0; shift -= 8) {
        *p++ = static_cast<std::uint8_t>(v >> static_cast<unsigned>(shift));
    }
}

void put_u64(std::uint8_t*& p, std::uint64_t v) noexcept {
    for (int shift = 56; shift >= 0; shift -= 8) {
        *p++ = static_cast<std::uint8_t>(v >> static_cast<unsigned>(shift));
    }
}

void put_f64(std::uint8_t*& p, double v) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);  // well-defined type punning
    put_u64(p, bits);
}

std::uint8_t get_u8(const std::uint8_t*& p) noexcept {
    return *p++;
}

std::uint16_t get_u16(const std::uint8_t*& p) noexcept {
    const auto v = static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8U) | p[1]);
    p += 2;
    return v;
}

std::uint32_t get_u32(const std::uint8_t*& p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8U) | *p++;
    }
    return v;
}

std::uint64_t get_u64(const std::uint8_t*& p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8U) | *p++;
    }
    return v;
}

double get_f64(const std::uint8_t*& p) noexcept {
    const std::uint64_t bits = get_u64(p);
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

// --- Minimal JSON scanner for the flat objects produced by to_json_line() -----

class JsonScanner {
public:
    explicit JsonScanner(std::string_view s) noexcept : s_(s) {}

    void skip_ws() noexcept {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) {
            ++pos_;
        }
    }

    [[nodiscard]] bool consume(char c) noexcept {
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool peek(char c) noexcept {
        skip_ws();
        return pos_ < s_.size() && s_[pos_] == c;
    }

    /// Reads a string without escape sequences (sufficient for our keys/values).
    [[nodiscard]] std::optional<std::string_view> string() noexcept {
        if (!consume('"')) {
            return std::nullopt;
        }
        const std::size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            if (s_[pos_] == '\\') {
                return std::nullopt;
            }
            ++pos_;
        }
        if (pos_ >= s_.size()) {
            return std::nullopt;
        }
        const std::string_view out = s_.substr(start, pos_ - start);
        ++pos_;
        return out;
    }

    /// Returns the raw text of a JSON number token.
    [[nodiscard]] std::string_view number_token() noexcept {
        skip_ws();
        const std::size_t start = pos_;
        while (pos_ < s_.size() && std::strchr("+-0123456789.eE", s_[pos_]) != nullptr && s_[pos_] != '\0') {
            ++pos_;
        }
        return s_.substr(start, pos_ - start);
    }

    [[nodiscard]] bool at_end() noexcept {
        skip_ws();
        return pos_ == s_.size();
    }

private:
    std::string_view s_;
    std::size_t pos_{0};
};

template <typename T>
[[nodiscard]] bool parse_number(std::string_view tok, T& out) noexcept {
    if (tok.empty()) {
        return false;
    }
    const char* const end = tok.data() + tok.size();
    const auto [ptr, ec] = std::from_chars(tok.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

}  // namespace

bool operator==(const TelemetrySample& a, const TelemetrySample& b) noexcept {
    for (const auto& f : kFields) {
        if (f.u64 != nullptr ? (a.*f.u64 != b.*f.u64) : (std::memcmp(&(a.*f.f64), &(b.*f.f64), sizeof(double)) != 0)) {
            return false;
        }
    }
    return true;
}

std::string_view to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::None: return "ok";
        case DecodeError::TooShort: return "too short";
        case DecodeError::BadMagic: return "bad magic";
        case DecodeError::BadVersion: return "unsupported version";
        case DecodeError::BadType: return "unknown message type";
        case DecodeError::BadLength: return "length mismatch";
        case DecodeError::BadCrc: return "CRC mismatch";
    }
    return "unknown";
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t seed) noexcept {
    std::uint32_t c = ~seed;
    for (std::size_t i = 0; i < len; ++i) {
        c = kCrcTable[(c ^ data[i]) & 0xFFU] ^ (c >> 8U);
    }
    return ~c;
}

Packet encode_binary(const TelemetrySample& s) noexcept {
    Packet pkt{};
    std::uint8_t* p = pkt.data();
    put_u32(p, wire::kMagic);
    put_u8(p, wire::kVersion);
    put_u8(p, static_cast<std::uint8_t>(MessageType::Telemetry));
    put_u16(p, static_cast<std::uint16_t>(wire::kPayloadSize));
    put_u64(p, s.seq);
    put_u64(p, s.mono_ns);
    for (const auto& f : kFields) {
        if (!f.in_payload) {
            continue;
        }
        if (f.u64 != nullptr) {
            put_u64(p, s.*f.u64);
        } else {
            put_f64(p, s.*f.f64);
        }
    }
    const std::uint32_t crc = crc32(pkt.data(), wire::kHeaderSize + wire::kPayloadSize);
    put_u32(p, crc);
    return pkt;
}

DecodeResult decode_binary(const std::uint8_t* data, std::size_t len) noexcept {
    DecodeResult r;
    if (data == nullptr || len < wire::kHeaderSize) {
        r.error = DecodeError::TooShort;
        return r;
    }
    const std::uint8_t* p = data;
    PacketHeader h;
    h.magic = get_u32(p);
    h.version = get_u8(p);
    const std::uint8_t type = get_u8(p);
    h.payload_len = get_u16(p);
    h.seq = get_u64(p);
    h.timestamp_ns = get_u64(p);

    if (h.magic != wire::kMagic) {
        r.error = DecodeError::BadMagic;
    } else if (h.version != wire::kVersion) {
        r.error = DecodeError::BadVersion;
    } else if (type != static_cast<std::uint8_t>(MessageType::Telemetry)) {
        r.error = DecodeError::BadType;
    } else if (h.payload_len != wire::kPayloadSize || len != wire::kPacketSize) {
        r.error = DecodeError::BadLength;
    } else {
        const std::size_t covered = wire::kHeaderSize + wire::kPayloadSize;
        const std::uint8_t* crc_ptr = data + covered;
        if (get_u32(crc_ptr) != crc32(data, covered)) {
            r.error = DecodeError::BadCrc;
        }
    }
    if (r.error != DecodeError::None) {
        return r;
    }

    r.sample.seq = h.seq;
    r.sample.mono_ns = h.timestamp_ns;
    for (const auto& f : kFields) {
        if (!f.in_payload) {
            continue;
        }
        if (f.u64 != nullptr) {
            r.sample.*f.u64 = get_u64(p);
        } else {
            r.sample.*f.f64 = get_f64(p);
        }
    }
    return r;
}

std::string to_json_line(const TelemetrySample& s) {
    std::string out;
    out.reserve(640);
    out += "{\"type\":\"telemetry\",\"v\":";
    out += std::to_string(wire::kVersion);
    char buf[64];
    for (const auto& f : kFields) {
        int n = 0;
        if (f.u64 != nullptr) {
            n = std::snprintf(buf, sizeof buf, ",\"%s\":%" PRIu64, f.name, s.*f.u64);
        } else {
            const double v = s.*f.f64;
            n = std::snprintf(buf, sizeof buf, ",\"%s\":%.3f", f.name, std::isfinite(v) ? v : 0.0);
        }
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n) < sizeof buf ? static_cast<std::size_t>(n) : sizeof buf - 1);
        }
    }
    out += "}\n";
    return out;
}

std::optional<TelemetrySample> parse_json_line(std::string_view line) {
    JsonScanner js(line);
    if (!js.consume('{')) {
        return std::nullopt;
    }
    TelemetrySample s;
    std::bitset<kFields.size()> seen;
    bool type_ok = false;
    bool first = true;
    while (!js.peek('}')) {
        if (!first && !js.consume(',')) {
            return std::nullopt;
        }
        first = false;
        const auto key = js.string();
        if (!key || !js.consume(':')) {
            return std::nullopt;
        }
        if (js.peek('"')) {
            const auto val = js.string();
            if (!val) {
                return std::nullopt;
            }
            if (*key == "type") {
                type_ok = (*val == "telemetry");
            }
            continue;
        }
        const std::string_view tok = js.number_token();
        if (*key == "v") {
            unsigned v = 0;
            if (!parse_number(tok, v) || v != wire::kVersion) {
                return std::nullopt;
            }
            continue;
        }
        for (std::size_t i = 0; i < kFields.size(); ++i) {
            const Field& f = kFields[i];
            if (*key != f.name) {
                continue;
            }
            const bool ok = f.u64 != nullptr ? parse_number(tok, s.*f.u64) : parse_number(tok, s.*f.f64);
            if (!ok) {
                return std::nullopt;
            }
            seen.set(i);
            break;
        }
    }
    if (!js.consume('}') || !js.at_end() || !type_ok || !seen.all()) {
        return std::nullopt;
    }
    return s;
}

}  // namespace etd
