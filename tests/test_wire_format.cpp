#include "telemetry_data.hpp"
#include "test_common.hpp"

#include <cstring>
#include <string>

namespace {

std::uint32_t crc_of(const char* s) {
    return etd::crc32(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s));
}

etd::TelemetrySample make_sample() {
    etd::TelemetrySample s;
    s.seq = 0x0102030405060708ULL;
    s.mono_ns = 123456789012345ULL;
    s.wall_ns = 1700000000123456789ULL;
    s.cpu_percent = 42.125;
    s.mem_total_kb = 16384000;
    s.mem_available_kb = 8192000;
    s.mem_used_kb = 8192000;
    s.swap_total_kb = 2048;
    s.swap_free_kb = 1024;
    s.uptime_s = 86400;
    s.load1 = 0.5;
    s.load5 = 0.25;
    s.load15 = 0.125;
    s.net_rx_bytes = 1000;
    s.net_tx_bytes = 2000;
    s.net_rx_packets = 10;
    s.net_tx_packets = 20;
    s.net_rx_errors = 1;
    s.net_tx_errors = 2;
    s.net_rx_drops = 3;
    s.net_tx_drops = 4;
    s.net_rx_bytes_per_s = 1234.5;
    s.net_tx_bytes_per_s = 6789.25;
    return s;
}

void test_crc32_vectors() {
    CHECK(crc_of("") == 0x00000000U);
    CHECK(crc_of("a") == 0xE8B7BE43U);
    CHECK(crc_of("123456789") == 0xCBF43926U);  // standard check value
    CHECK(crc_of("The quick brown fox jumps over the lazy dog") == 0x414FA339U);
    // Incremental computation equals one-shot.
    const char* msg = "123456789";
    const auto* p = reinterpret_cast<const std::uint8_t*>(msg);
    CHECK(etd::crc32(p + 4, 5, etd::crc32(p, 4)) == 0xCBF43926U);
}

void test_binary_roundtrip() {
    const auto s = make_sample();
    const etd::Packet pkt = etd::encode_binary(s);
    CHECK(pkt.size() == 196);
    // Header is big-endian: magic "TLMY", version, type, payload length 168, seq.
    CHECK(pkt[0] == 0x54 && pkt[1] == 0x4C && pkt[2] == 0x4D && pkt[3] == 0x59);
    CHECK(pkt[4] == etd::wire::kVersion && pkt[5] == 1);
    CHECK(pkt[6] == 0x00 && pkt[7] == 168);
    CHECK(pkt[8] == 0x01 && pkt[15] == 0x08);

    const auto r = etd::decode_binary(pkt.data(), pkt.size());
    CHECK(r.ok());
    CHECK(r.sample == s);
}

void test_binary_rejections() {
    const etd::Packet good = etd::encode_binary(make_sample());

    CHECK(etd::decode_binary(good.data(), 10).error == etd::DecodeError::TooShort);
    CHECK(etd::decode_binary(nullptr, 0).error == etd::DecodeError::TooShort);
    CHECK(etd::decode_binary(good.data(), good.size() - 1).error == etd::DecodeError::BadLength);

    auto bad = good;
    bad[0] ^= 0xFFU;
    CHECK(etd::decode_binary(bad.data(), bad.size()).error == etd::DecodeError::BadMagic);
    bad = good;
    bad[4] = 99;
    CHECK(etd::decode_binary(bad.data(), bad.size()).error == etd::DecodeError::BadVersion);
    bad = good;
    bad[5] = 7;
    CHECK(etd::decode_binary(bad.data(), bad.size()).error == etd::DecodeError::BadType);
    bad = good;
    bad[7] = 100;
    CHECK(etd::decode_binary(bad.data(), bad.size()).error == etd::DecodeError::BadLength);
    // Every single-bit flip in the payload or CRC must be detected.
    for (std::size_t i = etd::wire::kHeaderSize; i < good.size(); ++i) {
        bad = good;
        bad[i] ^= 0x10U;
        CHECK(etd::decode_binary(bad.data(), bad.size()).error == etd::DecodeError::BadCrc);
    }
    CHECK(etd::to_string(etd::DecodeError::BadCrc) == "CRC mismatch");
}

void test_json_shape() {
    const auto s = make_sample();
    const std::string line = etd::to_json_line(s);
    CHECK(line.rfind("{\"type\":\"telemetry\",\"v\":1,\"seq\":", 0) == 0);
    CHECK(line.size() >= 2 && line.substr(line.size() - 2) == "}\n");
    CHECK(line.find('\n') == line.size() - 1);  // exactly one line
    for (const char* key : {"\"mono_ns\":",
                            "\"wall_ns\":",
                            "\"cpu_pct\":42.125",
                            "\"mem_total_kb\":16384000",
                            "\"mem_avail_kb\":",
                            "\"mem_used_kb\":",
                            "\"swap_total_kb\":",
                            "\"swap_free_kb\":",
                            "\"uptime_s\":86400",
                            "\"load1\":0.500",
                            "\"load5\":",
                            "\"load15\":",
                            "\"net_rx_bytes\":",
                            "\"net_tx_bytes\":",
                            "\"net_rx_packets\":",
                            "\"net_tx_packets\":",
                            "\"net_rx_errors\":",
                            "\"net_tx_errors\":",
                            "\"net_rx_drops\":",
                            "\"net_tx_drops\":",
                            "\"net_rx_Bps\":1234.500",
                            "\"net_tx_Bps\":6789.250"}) {
        CHECK(line.find(key) != std::string::npos);
    }

    const auto parsed = etd::parse_json_line(line);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->seq == s.seq && parsed->wall_ns == s.wall_ns && parsed->net_tx_drops == 4);
        CHECK_NEAR(parsed->cpu_percent, s.cpu_percent, 1e-3);
        CHECK_NEAR(parsed->net_tx_bytes_per_s, s.net_tx_bytes_per_s, 1e-3);
    }

    CHECK(!etd::parse_json_line("").has_value());
    CHECK(!etd::parse_json_line("{}").has_value());
    CHECK(!etd::parse_json_line("not json").has_value());
    CHECK(!etd::parse_json_line(line.substr(0, line.size() / 2)).has_value());
    std::string wrong_type = line;
    wrong_type.replace(9, 9, "xxxxxxxxx");
    CHECK(!etd::parse_json_line(wrong_type).has_value());
}

}  // namespace

int main() {
    test_crc32_vectors();
    test_binary_roundtrip();
    test_binary_rejections();
    test_json_shape();
    return etd_test::finish("wire_format");
}
