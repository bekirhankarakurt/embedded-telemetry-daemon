#include "telemetry_data.hpp"
#include "test_common.hpp"
#include "udp_sender.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace {

void test_validation() {
    CHECK(!etd::validate_udp_config({"127.0.0.1", 5000, false}).has_value());
    CHECK(!etd::validate_udp_config({"255.255.255.255", 5000, true}).has_value());
    CHECK(etd::validate_udp_config({"255.255.255.255", 5000, false}).has_value());
    CHECK(etd::validate_udp_config({"localhost", 5000, false}).has_value());
    CHECK(etd::validate_udp_config({"1.2.3", 5000, false}).has_value());
    CHECK(etd::validate_udp_config({"256.1.1.1", 5000, false}).has_value());
    CHECK(etd::validate_udp_config({"0.0.0.0", 5000, false}).has_value());
    CHECK(etd::validate_udp_config({"127.0.0.1", 0, false}).has_value());

    bool threw = false;
    try {
        etd::UdpSender bad({"not-an-ip", 1, false});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_loopback_roundtrip() {
    const int rx = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(rx >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001U);
    addr.sin_port = 0;  // ephemeral
    CHECK(::bind(rx, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
    socklen_t alen = sizeof addr;
    CHECK(::getsockname(rx, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);

    etd::UdpSender sender({"127.0.0.1", ntohs(addr.sin_port), false});
    etd::UdpSender moved(std::move(sender));  // ownership transfers
    CHECK(!sender.valid() && moved.valid());  // NOLINT(bugprone-use-after-move): verifying moved-from state

    etd::TelemetrySample s;
    s.seq = 42;
    s.cpu_percent = 12.5;
    const etd::Packet pkt = etd::encode_binary(s);
    CHECK(moved.send(pkt.data(), pkt.size()) == etd::SendResult::Sent);
    CHECK(moved.sent() == 1 && moved.dropped() == 0 && moved.errors() == 0);

    pollfd pfd{rx, POLLIN, 0};
    CHECK(::poll(&pfd, 1, 2000) == 1);
    std::array<std::uint8_t, 512> buf{};
    const ssize_t n = ::recv(rx, buf.data(), buf.size(), 0);
    CHECK(n == static_cast<ssize_t>(etd::wire::kPacketSize));
    const auto r = etd::decode_binary(buf.data(), static_cast<std::size_t>(n > 0 ? n : 0));
    CHECK(r.ok() && r.sample == s);

    // Moved-from sender reports an error instead of touching a closed fd.
    CHECK(sender.send(pkt.data(), pkt.size()) == etd::SendResult::Error);  // NOLINT(bugprone-use-after-move)
    ::close(rx);
}

void test_broadcast_socket() {
    etd::UdpSender b({"255.255.255.255", 9, true});
    CHECK(b.valid());
}

}  // namespace

int main() {
    test_validation();
    test_loopback_roundtrip();
    test_broadcast_socket();
    return etd_test::finish("udp_sender");
}
