#include "collector.hpp"

#include <sys/sysinfo.h>

#include <cmath>
#include <fstream>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace etd {
namespace {

std::chrono::nanoseconds period_from_rate(double rate_hz) {
    if (!(rate_hz > 0.0) || rate_hz > 1000.0 || !std::isfinite(rate_hz)) {
        throw std::invalid_argument("collector rate must be in (0, 1000] Hz");
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / rate_hz));
}

std::uint64_t to_ns(std::chrono::nanoseconds d) noexcept {
    return d.count() > 0 ? static_cast<std::uint64_t>(d.count()) : 0U;
}

std::string_view trim(std::string_view s) noexcept {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::optional<CpuTimes> parse_proc_stat(std::istream& in) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu ", 0) != 0) {
            continue;
        }
        std::istringstream ss(line.substr(4));
        CpuTimes t;
        // user nice system idle are mandatory; later columns appeared in newer kernels.
        if (!(ss >> t.user >> t.nice >> t.system >> t.idle)) {
            return std::nullopt;
        }
        std::uint64_t* const optional_cols[] = {&t.iowait, &t.irq, &t.softirq, &t.steal};
        for (std::uint64_t* col : optional_cols) {
            if (!(ss >> *col)) {
                *col = 0;
                break;
            }
        }
        return t;
    }
    return std::nullopt;
}

std::optional<MemInfo> parse_meminfo(std::istream& in) {
    MemInfo m;
    bool have_total = false;
    bool have_available = false;
    std::uint64_t free_kb = 0;
    std::uint64_t buffers_kb = 0;
    std::uint64_t cached_kb = 0;

    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string_view key(line.data(), colon);
        std::istringstream ss(line.substr(colon + 1));
        std::uint64_t value = 0;
        if (!(ss >> value)) {
            continue;
        }
        if (key == "MemTotal") {
            m.total_kb = value;
            have_total = true;
        } else if (key == "MemAvailable") {
            m.available_kb = value;
            have_available = true;
        } else if (key == "MemFree") {
            free_kb = value;
        } else if (key == "Buffers") {
            buffers_kb = value;
        } else if (key == "Cached") {
            cached_kb = value;
        } else if (key == "SwapTotal") {
            m.swap_total_kb = value;
        } else if (key == "SwapFree") {
            m.swap_free_kb = value;
        }
    }
    if (!have_total) {
        return std::nullopt;
    }
    if (!have_available) {
        m.available_kb = free_kb + buffers_kb + cached_kb;
    }
    if (m.available_kb > m.total_kb) {
        m.available_kb = m.total_kb;
    }
    return m;
}

std::optional<NetCounters> parse_net_dev(std::istream& in) {
    std::string line;
    // Two header lines: "Inter-| Receive ..." and " face |bytes ...".
    if (!std::getline(in, line) || !std::getline(in, line) || line.find('|') == std::string::npos) {
        return std::nullopt;
    }
    NetCounters sum;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (trim(std::string_view(line).substr(0, colon)) == "lo") {
            continue;
        }
        std::istringstream ss(line.substr(colon + 1));
        std::uint64_t v[16] = {};
        bool ok = true;
        for (auto& x : v) {
            if (!(ss >> x)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;  // malformed interface line: skip it rather than corrupt the totals
        }
        sum.rx_bytes += v[0];
        sum.rx_packets += v[1];
        sum.rx_errors += v[2];
        sum.rx_drops += v[3];
        sum.tx_bytes += v[8];
        sum.tx_packets += v[9];
        sum.tx_errors += v[10];
        sum.tx_drops += v[11];
    }
    return sum;
}

double cpu_usage_percent(const CpuTimes& prev, const CpuTimes& cur) noexcept {
    const std::uint64_t total = counter_delta(prev.total(), cur.total());
    if (total == 0) {
        return 0.0;
    }
    const std::uint64_t idle = counter_delta(prev.idle_total(), cur.idle_total());
    const std::uint64_t busy = idle < total ? total - idle : 0;
    return 100.0 * static_cast<double>(busy) / static_cast<double>(total);
}

Collector::Collector(CollectorConfig cfg, Sink sink)
    : cfg_(std::move(cfg)), sink_(std::move(sink)), period_(period_from_rate(cfg_.rate_hz)) {
    if (!sink_) {
        throw std::invalid_argument("collector sink must not be empty");
    }
}

Collector::~Collector() {
    stop();
}

void Collector::start() {
    if (thread_.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lk(mtx_);
        stop_requested_ = false;
    }
    // Baseline so the first emitted sample already carries meaningful deltas.
    static_cast<void>(sample_once());
    next_seq_ = 0;
    thread_ = std::thread(&Collector::run, this);
}

void Collector::stop() noexcept {
    {
        const std::lock_guard<std::mutex> lk(mtx_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::optional<TelemetrySample> Collector::sample_once() {
    const auto mono = std::chrono::steady_clock::now();
    const auto wall = std::chrono::system_clock::now();

    TelemetrySample s;
    s.seq = next_seq_++;
    s.mono_ns = to_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(mono.time_since_epoch()));
    s.wall_ns = to_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(wall.time_since_epoch()));

    const double dt = prev_time_ == std::chrono::steady_clock::time_point{}
                          ? 0.0
                          : std::chrono::duration<double>(mono - prev_time_).count();
    prev_time_ = mono;

    unsigned failures = 0;

    std::ifstream stat_file(cfg_.proc_root + "/stat");
    if (auto cpu = parse_proc_stat(stat_file)) {
        if (prev_cpu_) {
            s.cpu_percent = cpu_usage_percent(*prev_cpu_, *cpu);
        }
        prev_cpu_ = cpu;
    } else {
        ++failures;
    }

    std::ifstream mem_file(cfg_.proc_root + "/meminfo");
    if (const auto mem = parse_meminfo(mem_file)) {
        s.mem_total_kb = mem->total_kb;
        s.mem_available_kb = mem->available_kb;
        s.mem_used_kb = mem->total_kb - mem->available_kb;
        s.swap_total_kb = mem->swap_total_kb;
        s.swap_free_kb = mem->swap_free_kb;
    } else {
        ++failures;
    }

    std::ifstream net_file(cfg_.proc_root + "/net/dev");
    if (auto net = parse_net_dev(net_file)) {
        s.net_rx_bytes = net->rx_bytes;
        s.net_tx_bytes = net->tx_bytes;
        s.net_rx_packets = net->rx_packets;
        s.net_tx_packets = net->tx_packets;
        s.net_rx_errors = net->rx_errors;
        s.net_tx_errors = net->tx_errors;
        s.net_rx_drops = net->rx_drops;
        s.net_tx_drops = net->tx_drops;
        if (prev_net_ && dt > 0.0) {
            s.net_rx_bytes_per_s = static_cast<double>(counter_delta(prev_net_->rx_bytes, net->rx_bytes)) / dt;
            s.net_tx_bytes_per_s = static_cast<double>(counter_delta(prev_net_->tx_bytes, net->tx_bytes)) / dt;
        }
        prev_net_ = net;
    } else {
        ++failures;
    }

    struct sysinfo si {};
    if (::sysinfo(&si) == 0) {
        constexpr double kLoadScale = 1 << SI_LOAD_SHIFT;
        s.uptime_s = si.uptime > 0 ? static_cast<std::uint64_t>(si.uptime) : 0U;
        s.load1 = static_cast<double>(si.loads[0]) / kLoadScale;
        s.load5 = static_cast<double>(si.loads[1]) / kLoadScale;
        s.load15 = static_cast<double>(si.loads[2]) / kLoadScale;
    } else {
        ++failures;
    }

    if (failures > 0) {
        read_failures_.fetch_add(1, std::memory_order_relaxed);
    }
    if (failures == 4) {
        return std::nullopt;
    }
    return s;
}

void Collector::run() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now() + period_;
    std::unique_lock<std::mutex> lk(mtx_);
    while (!stop_requested_) {
        if (cv_.wait_until(lk, next, [this] { return stop_requested_; })) {
            break;
        }
        lk.unlock();
        try {
            if (const auto s = sample_once()) {
                sink_(*s);
            }
        } catch (...) {
            // A failing sample or sink must never take the daemon down.
            read_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        lk.lock();

        next += period_;
        const auto now = clock::now();
        if (next <= now) {
            // Overrun: skip missed ticks instead of bursting to catch up.
            const auto behind = now - next;
            next += period_ * (behind / period_ + 1);
        }
    }
}

}  // namespace etd
