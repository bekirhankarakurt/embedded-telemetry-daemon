/**
 * @file telemetry_listener.cpp
 * @brief Ground-station receiver: decodes binary or JSON telemetry, validates
 *        integrity and reports sequence gaps / reordering live.
 */
#include "telemetry_data.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal flag must be lock-free");

extern "C" void on_signal(int /*signo*/) {
    g_stop.store(true);
}

/// RAII socket descriptor.
class Socket {
public:
    explicit Socket(int fd) noexcept : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) = delete;
    Socket& operator=(Socket&&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

/// Tracks sequence continuity of the incoming stream.
class SeqTracker {
public:
    enum class Event : std::uint8_t { First, InOrder, Gap, Reordered, Restart };

    Event observe(std::uint64_t seq) noexcept {
        if (!started_) {
            started_ = true;
            expected_ = seq + 1;
            return Event::First;
        }
        if (seq == expected_) {
            ++expected_;
            return Event::InOrder;
        }
        if (seq > expected_) {
            last_gap_ = seq - expected_;
            lost_ += last_gap_;
            expected_ = seq + 1;
            return Event::Gap;
        }
        if (seq == 0) {  // sender restarted
            ++restarts_;
            expected_ = 1;
            return Event::Restart;
        }
        ++reordered_;
        return Event::Reordered;
    }

    [[nodiscard]] std::uint64_t lost() const noexcept { return lost_; }
    [[nodiscard]] std::uint64_t last_gap() const noexcept { return last_gap_; }
    [[nodiscard]] std::uint64_t reordered() const noexcept { return reordered_; }
    [[nodiscard]] std::uint64_t restarts() const noexcept { return restarts_; }

private:
    bool started_{false};
    std::uint64_t expected_{0};
    std::uint64_t lost_{0};
    std::uint64_t last_gap_{0};
    std::uint64_t reordered_{0};
    std::uint64_t restarts_{0};
};

struct Counters {
    std::uint64_t binary{0};
    std::uint64_t json{0};
    std::uint64_t invalid{0};
    std::array<std::uint64_t, 7> by_error{};
};

void print_usage() {
    std::printf(
        "Usage: telemetry_listener [--port <port>] [--bind <ipv4>] [--quiet]\n"
        "\n"
        "Receives embedded-telemetry-daemon datagrams (binary or JSON, auto-detected),\n"
        "verifies magic/version/length/CRC-32 and reports sequence gaps and reordering.\n"
        "\n"
        "  --port <port>   UDP port to listen on      (default 5000)\n"
        "  --bind <ipv4>   local address to bind      (default 0.0.0.0)\n"
        "  --quiet         only print anomalies and the final summary\n"
        "  --help          show this help\n");
}

std::uint64_t now_wall_ns() noexcept {
    const auto d = std::chrono::system_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    return ns > 0 ? static_cast<std::uint64_t>(ns) : 0U;
}

void print_sample(const char* fmt, const etd::TelemetrySample& s) {
    const std::uint64_t now = now_wall_ns();
    const double age_ms =
        now >= s.wall_ns ? static_cast<double>(now - s.wall_ns) / 1e6 : -static_cast<double>(s.wall_ns - now) / 1e6;
    std::printf("[%s] seq=%-6" PRIu64 " cpu=%5.1f%% mem=%" PRIu64 "/%" PRIu64
                " MiB load=%.2f/%.2f/%.2f rx=%.1f KiB/s tx=%.1f KiB/s up=%" PRIu64 "s age=%.1fms\n",
                fmt, s.seq, s.cpu_percent, s.mem_used_kb / 1024U, s.mem_total_kb / 1024U, s.load1, s.load5, s.load15,
                s.net_rx_bytes_per_s / 1024.0, s.net_tx_bytes_per_s / 1024.0, s.uptime_s, age_ms);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 5000;
    std::string bind_addr = "0.0.0.0";
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
        } else if ((arg == "--port" || arg == "--bind") && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (arg == "--bind") {
                bind_addr = std::string(v);
                continue;
            }
            unsigned long p = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), p);
            if (ec != std::errc{} || ptr != v.data() + v.size() || p == 0 || p > 65535) {
                std::fprintf(stderr, "error: --port must be in 1..65535\n");
                return 64;
            }
            port = static_cast<std::uint16_t>(p);
        } else {
            std::fprintf(stderr, "error: invalid argument '%s' (see --help)\n", argv[i]);
            return 64;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "error: invalid bind address '%s'\n", bind_addr.c_str());
        return 64;
    }

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    const Socket sock(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (sock.get() < 0) {
        std::perror("socket");
        return 71;
    }
    if (::bind(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        std::perror("bind");
        return 71;
    }
    std::fprintf(stderr, "listening on %s:%u (Ctrl+C to stop)\n", bind_addr.c_str(), static_cast<unsigned>(port));

    SeqTracker seq;
    Counters cnt;
    std::array<std::uint8_t, 2048> buf{};

    while (!g_stop.load()) {
        pollfd pfd{sock.get(), POLLIN, 0};
        const int rc = ::poll(&pfd, 1, 250);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            return 71;
        }
        if (rc == 0) {
            continue;
        }
        const ssize_t n = ::recv(sock.get(), buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            std::perror("recv");
            return 71;
        }
        const auto len = static_cast<std::size_t>(n);

        etd::TelemetrySample sample;
        const char* fmt = "bin ";
        if (len > 0 && buf[0] == '{') {
            const std::string_view text(reinterpret_cast<const char*>(buf.data()), len);
            const auto parsed = etd::parse_json_line(text);
            if (!parsed) {
                ++cnt.invalid;
                std::printf("!! invalid JSON datagram (%zu bytes)\n", len);
                continue;
            }
            sample = *parsed;
            fmt = "json";
            ++cnt.json;
        } else {
            const etd::DecodeResult r = etd::decode_binary(buf.data(), len);
            if (!r.ok()) {
                ++cnt.invalid;
                ++cnt.by_error[static_cast<std::size_t>(r.error)];
                std::printf("!! rejected binary datagram (%zu bytes): %s\n", len,
                            std::string(etd::to_string(r.error)).c_str());
                continue;
            }
            sample = r.sample;
            ++cnt.binary;
        }

        switch (seq.observe(sample.seq)) {
            case SeqTracker::Event::Gap:
                std::printf("!! sequence gap: %" PRIu64 " packet(s) lost before seq %" PRIu64 "\n", seq.last_gap(),
                            sample.seq);
                break;
            case SeqTracker::Event::Reordered:
                std::printf("!! out-of-order or duplicate packet: seq %" PRIu64 "\n", sample.seq);
                break;
            case SeqTracker::Event::Restart: std::printf("!! sender restarted (seq reset to 0)\n"); break;
            case SeqTracker::Event::First:
            case SeqTracker::Event::InOrder: break;
        }
        if (!quiet) {
            print_sample(fmt, sample);
        }
        std::fflush(stdout);
    }

    std::printf("\nsummary: binary=%" PRIu64 " json=%" PRIu64 " invalid=%" PRIu64 " lost=%" PRIu64 " reordered=%" PRIu64
                " restarts=%" PRIu64 "\n",
                cnt.binary, cnt.json, cnt.invalid, seq.lost(), seq.reordered(), seq.restarts());
    for (std::size_t i = 1; i < cnt.by_error.size(); ++i) {
        if (cnt.by_error[i] > 0) {
            std::printf("  rejected (%s): %" PRIu64 "\n",
                        std::string(etd::to_string(static_cast<etd::DecodeError>(i))).c_str(), cnt.by_error[i]);
        }
    }
    return 0;
}
