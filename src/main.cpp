/**
 * @file main.cpp
 * @brief embedded-telemetry-daemon entry point: CLI parsing, wiring and graceful shutdown.
 */
#include "collector.hpp"
#include "rolling_logger.hpp"
#include "telemetry_data.hpp"
#include "udp_sender.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

/// Process exit codes (values follow BSD sysexits.h conventions).
enum class ExitCode : int {
    Ok = 0,         ///< Normal termination, including SIGINT/SIGTERM shutdown.
    Usage = 64,     ///< Invalid command line (EX_USAGE).
    Software = 70,  ///< Unexpected internal error (EX_SOFTWARE).
    OsError = 71,   ///< Socket/pipe/thread creation failed (EX_OSERR).
};

[[nodiscard]] int to_int(ExitCode c) noexcept {
    return static_cast<int>(c);
}

enum class WireFormat : std::uint8_t { Json, Binary };

struct Options {
    double rate_hz{1.0};
    std::string dest{"127.0.0.1"};
    std::uint16_t port{5000};
    bool broadcast{false};
    WireFormat format{WireFormat::Binary};
    std::string log_file{"telemetry.jsonl"};
    std::size_t log_max_kb{1024};
    unsigned log_backups{3};
    bool no_log{false};
    bool no_udp{false};
    double duration_s{0.0};
    bool verbose{false};
};

// ---- Signal handling (async-signal-safe: lock-free atomic + write(2) only) ----

static_assert(std::atomic<bool>::is_always_lock_free, "signal flag must be lock-free");
std::atomic<bool> g_stop{false};
std::atomic<int> g_wake_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free, "wake fd must be lock-free");

extern "C" void on_signal(int /*signo*/) {
    const int saved_errno = errno;
    g_stop.store(true);
    const int fd = g_wake_fd.load();
    if (fd >= 0) {
        const char byte = 1;
        // Non-blocking; if the pipe is full the reader is already being woken.
        const ssize_t rc = ::write(fd, &byte, 1);
        static_cast<void>(rc);
    }
    errno = saved_errno;
}

/// RAII owner of a file descriptor.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    UniqueFd(UniqueFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) {
            UniqueFd tmp(std::move(o));
            std::swap(fd_, tmp.fd_);
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_{-1};
};

// ---- CLI ----------------------------------------------------------------------

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "Usage: embedded-telemetry-daemon [options]\n"
                 "\n"
                 "Samples CPU, memory, load and network statistics, records them as JSON lines\n"
                 "and streams them over UDP to a ground station.\n"
                 "\n"
                 "Options:\n"
                 "  --rate-hz <hz>       sampling rate, 0 < hz <= 1000          (default 1)\n"
                 "  --dest <ipv4>        UDP destination address                (default 127.0.0.1)\n"
                 "  --port <port>        UDP destination port, 1..65535         (default 5000)\n"
                 "  --broadcast          enable SO_BROADCAST (e.g. --dest 192.168.1.255)\n"
                 "  --format <fmt>       UDP payload: binary | json             (default binary)\n"
                 "  --log-file <path>    JSON-lines log file                    (default telemetry.jsonl)\n"
                 "  --log-max-kb <kb>    rotate log above this size, >= 1       (default 1024)\n"
                 "  --log-backups <n>    rotated files to keep, 0..100          (default 3)\n"
                 "  --no-log             disable file logging\n"
                 "  --no-udp             disable the UDP downlink\n"
                 "  --duration <s>       stop after s seconds, 0 = run forever  (default 0)\n"
                 "  --verbose            print one status line per second to stderr\n"
                 "  --help               show this help and exit\n"
                 "  --version            show version and exit\n"
                 "\n"
                 "Exit codes: 0 ok, 64 usage error, 70 internal error, 71 OS resource error.\n");
}

template <typename T>
[[nodiscard]] bool parse_int(std::string_view s, T& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

[[nodiscard]] bool parse_double(const std::string& s, double& out) noexcept {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end != s.c_str() + s.size() || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

/// Result of parsing: options, or an exit code for --help/--version/errors.
struct ParseOutcome {
    std::optional<Options> opts;
    ExitCode exit{ExitCode::Ok};
};

[[nodiscard]] ParseOutcome usage_error(const std::string& msg) {
    std::fprintf(stderr, "error: %s\nTry 'embedded-telemetry-daemon --help'.\n", msg.c_str());
    return {std::nullopt, ExitCode::Usage};
}

[[nodiscard]] ParseOutcome parse_args(int argc, char** argv) {
    Options o;
    bool dest_given = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::optional<std::string> inline_value;
        if (const auto eq = arg.find('='); arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            inline_value = arg.substr(eq + 1);
            arg.resize(eq);
        }
        auto value = [&]() -> std::optional<std::string> {
            if (inline_value) {
                return inline_value;
            }
            if (i + 1 < argc) {
                return std::string(argv[++i]);
            }
            return std::nullopt;
        };
        auto flag = [&](bool& target) -> bool {
            if (inline_value) {
                return false;
            }
            target = true;
            return true;
        };

        if (arg == "--help" || arg == "-h") {
            print_usage(stdout);
            return {std::nullopt, ExitCode::Ok};
        }
        if (arg == "--version") {
            std::printf("embedded-telemetry-daemon %s (wire format v%u)\n", ETD_VERSION,
                        static_cast<unsigned>(etd::wire::kVersion));
            return {std::nullopt, ExitCode::Ok};
        }
        if (arg == "--broadcast" || arg == "--no-log" || arg == "--no-udp" || arg == "--verbose") {
            bool& target = arg == "--broadcast" ? o.broadcast
                           : arg == "--no-log"  ? o.no_log
                           : arg == "--no-udp"  ? o.no_udp
                                                : o.verbose;
            if (!flag(target)) {
                return usage_error(arg + " does not take a value");
            }
            continue;
        }

        const bool takes_value = arg == "--rate-hz" || arg == "--dest" || arg == "--port" || arg == "--format" ||
                                 arg == "--log-file" || arg == "--log-max-kb" || arg == "--log-backups" ||
                                 arg == "--duration";
        if (!takes_value) {
            return usage_error("unknown option '" + arg + "'");
        }
        const auto v = value();
        if (!v) {
            return usage_error(arg + " requires a value");
        }
        if (arg == "--rate-hz") {
            if (!parse_double(*v, o.rate_hz) || !(o.rate_hz > 0.0) || o.rate_hz > 1000.0) {
                return usage_error("--rate-hz must be a number in (0, 1000]");
            }
        } else if (arg == "--dest") {
            o.dest = *v;
            dest_given = true;
        } else if (arg == "--port") {
            unsigned long port = 0;
            if (!parse_int(*v, port) || port == 0 || port > 65535) {
                return usage_error("--port must be an integer in 1..65535");
            }
            o.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--format") {
            if (*v == "json") {
                o.format = WireFormat::Json;
            } else if (*v == "binary") {
                o.format = WireFormat::Binary;
            } else {
                return usage_error("--format must be 'json' or 'binary'");
            }
        } else if (arg == "--log-file") {
            if (v->empty()) {
                return usage_error("--log-file must not be empty");
            }
            o.log_file = *v;
        } else if (arg == "--log-max-kb") {
            if (!parse_int(*v, o.log_max_kb) || o.log_max_kb == 0 || o.log_max_kb > 4U * 1024U * 1024U) {
                return usage_error("--log-max-kb must be an integer in 1..4194304");
            }
        } else if (arg == "--log-backups") {
            if (!parse_int(*v, o.log_backups) || o.log_backups > 100) {
                return usage_error("--log-backups must be an integer in 0..100");
            }
        } else if (arg == "--duration") {
            if (!parse_double(*v, o.duration_s) || o.duration_s < 0.0) {
                return usage_error("--duration must be a non-negative number of seconds");
            }
        }
    }

    if (o.no_log && o.no_udp) {
        return usage_error("--no-log and --no-udp together leave nothing to do");
    }
    if (o.broadcast && !dest_given) {
        o.dest = "255.255.255.255";
    }
    if (!o.no_udp) {
        const etd::UdpConfig ucfg{o.dest, o.port, o.broadcast};
        if (auto err = etd::validate_udp_config(ucfg)) {
            return usage_error(*err);
        }
    }
    return {o, ExitCode::Ok};
}

/// Block or unblock SIGINT/SIGTERM for the calling thread.
void set_shutdown_signals_blocked(bool blocked) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (const int rc = pthread_sigmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, nullptr); rc != 0) {
        throw std::system_error(rc, std::generic_category(), "pthread_sigmask");
    }
}

[[nodiscard]] UniqueFd install_signal_handlers() {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
    UniqueFd read_end(fds[0]);
    g_wake_fd.store(fds[1]);  // write end intentionally lives until process exit

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (const int sig : {SIGINT, SIGTERM}) {
        if (::sigaction(sig, &sa, nullptr) != 0) {
            throw std::system_error(errno, std::generic_category(), "sigaction");
        }
    }
    return read_end;
}

/// Runtime status shared between the collector thread and the main thread.
struct Status {
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> last_seq{0};
    std::atomic<double> last_cpu{0.0};
    std::atomic<bool> udp_error_reported{false};
};

void print_status(const char* prefix, const Status& st, const etd::UdpSender* udp, const etd::RollingLogger* log,
                  const etd::Collector& col) {
    std::fprintf(stderr, "%s samples=%" PRIu64 " seq=%" PRIu64 " cpu=%.1f%% read_fail=%" PRIu64, prefix,
                 st.samples.load(), st.last_seq.load(), st.last_cpu.load(), col.read_failures());
    if (udp != nullptr) {
        std::fprintf(stderr, " | udp sent=%" PRIu64 " dropped=%" PRIu64 " err=%" PRIu64, udp->sent(), udp->dropped(),
                     udp->errors());
    }
    if (log != nullptr) {
        std::fprintf(stderr, " | log lines=%" PRIu64 " rot=%" PRIu64 " err=%" PRIu64 " overflow=%" PRIu64,
                     log->file().lines_written(), log->file().rotations(), log->file().errors(),
                     log->queue_overflows());
    }
    std::fputc('\n', stderr);
}

int run(const Options& o) {
    UniqueFd wake = install_signal_handlers();

    std::unique_ptr<etd::UdpSender> udp;
    if (!o.no_udp) {
        udp = std::make_unique<etd::UdpSender>(etd::UdpConfig{o.dest, o.port, o.broadcast});
    }
    std::unique_ptr<etd::RollingLogger> logger;
    if (!o.no_log) {
        etd::LoggerConfig lcfg;
        lcfg.path = o.log_file;
        lcfg.max_bytes = o.log_max_kb * 1024U;
        lcfg.backups = o.log_backups;
        logger = std::make_unique<etd::RollingLogger>(lcfg);
    }

    Status status;
    const WireFormat fmt = o.format;
    etd::UdpSender* const udp_ptr = udp.get();
    etd::RollingLogger* const log_ptr = logger.get();

    // Runs on the collector thread: non-blocking UDP send + O(1) enqueue for the logger.
    auto sink = [&status, fmt, udp_ptr, log_ptr](const etd::TelemetrySample& s) {
        status.samples.fetch_add(1);
        status.last_seq.store(s.seq);
        status.last_cpu.store(s.cpu_percent);
        if (udp_ptr != nullptr) {
            etd::SendResult r{};
            if (fmt == WireFormat::Binary) {
                const etd::Packet pkt = etd::encode_binary(s);
                r = udp_ptr->send(pkt.data(), pkt.size());
            } else {
                const std::string line = etd::to_json_line(s);
                r = udp_ptr->send(line.data(), line.size());
            }
            if (r == etd::SendResult::Error && !status.udp_error_reported.exchange(true)) {
                std::fprintf(stderr, "udp: send failed: %s (further errors counted silently)\n",
                             std::strerror(udp_ptr->last_errno()));
            }
        }
        if (log_ptr != nullptr) {
            log_ptr->submit(s);
        }
    };

    etd::Collector collector(etd::CollectorConfig{o.rate_hz, "/proc"}, sink);

    // Worker threads inherit a mask with SIGINT/SIGTERM blocked, so signals are
    // always delivered to the main thread, which owns the shutdown sequence.
    set_shutdown_signals_blocked(true);
    if (logger) {
        logger->start();
    }
    collector.start();
    set_shutdown_signals_blocked(false);

    std::fprintf(
        stderr, "embedded-telemetry-daemon %s: %.3g Hz, udp=%s, log=%s\n", ETD_VERSION, o.rate_hz,
        udp ? (o.dest + ":" + std::to_string(o.port) + (fmt == WireFormat::Binary ? " (binary)" : " (json)")).c_str()
            : "off",
        logger ? o.log_file.c_str() : "off");

    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    const bool bounded = o.duration_s > 0.0;
    const auto deadline =
        started + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(o.duration_s));

    while (!g_stop.load()) {
        auto timeout = std::chrono::milliseconds(1000);
        if (bounded) {
            const auto now = clock::now();
            if (now >= deadline) {
                break;
            }
            timeout = std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) +
                                            std::chrono::milliseconds(1));
        }
        pollfd pfd{wake.get(), POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (rc < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (o.verbose && !g_stop.load()) {
            print_status("status:", status, udp_ptr, log_ptr, collector);
        }
    }

    // Ordered shutdown: stop producing, then drain the log queue.
    collector.stop();
    if (logger) {
        logger->stop();
    }
    print_status(g_stop.load() ? "shutdown (signal):" : "shutdown (duration):", status, udp_ptr, log_ptr, collector);
    return to_int(ExitCode::Ok);
}

}  // namespace

int main(int argc, char** argv) {
    const ParseOutcome parsed = parse_args(argc, argv);
    if (!parsed.opts) {
        return to_int(parsed.exit);
    }
    try {
        return run(*parsed.opts);
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return to_int(ExitCode::OsError);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return to_int(ExitCode::Software);
    }
}
