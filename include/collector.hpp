/**
 * @file collector.hpp
 * @brief /proc and sysinfo() based system metrics collector running on its own thread.
 */
#pragma once

#include "telemetry_data.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace etd {

/// Cumulative CPU jiffies from the aggregate "cpu" line of /proc/stat.
struct CpuTimes {
    std::uint64_t user{0}, nice{0}, system{0}, idle{0}, iowait{0}, irq{0}, softirq{0}, steal{0};
    /// Sum of all accounted jiffies (guest time is already included in user/nice).
    [[nodiscard]] std::uint64_t total() const noexcept {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    /// Jiffies spent idle or waiting for I/O.
    [[nodiscard]] std::uint64_t idle_total() const noexcept { return idle + iowait; }
};

/// Subset of /proc/meminfo (values in KiB).
struct MemInfo {
    std::uint64_t total_kb{0};
    std::uint64_t available_kb{0};
    std::uint64_t swap_total_kb{0};
    std::uint64_t swap_free_kb{0};
};

/// Network counters summed over all non-loopback interfaces of /proc/net/dev.
struct NetCounters {
    std::uint64_t rx_bytes{0}, rx_packets{0}, rx_errors{0}, rx_drops{0};
    std::uint64_t tx_bytes{0}, tx_packets{0}, tx_errors{0}, tx_drops{0};
};

/// Parse the aggregate "cpu" line of /proc/stat.
[[nodiscard]] std::optional<CpuTimes> parse_proc_stat(std::istream& in);

/**
 * Parse /proc/meminfo. Uses MemAvailable when present (Linux >= 3.14), otherwise
 * falls back to MemFree + Buffers + Cached as an estimate.
 */
[[nodiscard]] std::optional<MemInfo> parse_meminfo(std::istream& in);

/// Parse /proc/net/dev, summing every interface except "lo".
[[nodiscard]] std::optional<NetCounters> parse_net_dev(std::istream& in);

/// CPU utilisation in percent between two snapshots; 0 if no time elapsed.
[[nodiscard]] double cpu_usage_percent(const CpuTimes& prev, const CpuTimes& cur) noexcept;

/// Counter delta that treats a decrease (wrap/reset/interface removal) as zero.
[[nodiscard]] constexpr std::uint64_t counter_delta(std::uint64_t prev, std::uint64_t cur) noexcept {
    return cur >= prev ? cur - prev : 0;
}

/// Collector configuration.
struct CollectorConfig {
    double rate_hz{1.0};             ///< Sampling frequency (0 < rate_hz <= 1000).
    std::string proc_root{"/proc"};  ///< Root of the proc filesystem (overridable for tests).
};

/**
 * Periodically samples system metrics on a dedicated thread and hands each
 * sample to a sink callback. Ticks are scheduled on an absolute steady_clock
 * timeline (sleep_until) so jitter does not accumulate; stop() wakes the thread
 * immediately via a condition variable.
 */
class Collector {
public:
    /// Called on the collector thread for every sample. Must not block for long.
    using Sink = std::function<void(const TelemetrySample&)>;

    /// @throws std::invalid_argument if the rate is out of range or the sink is empty.
    Collector(CollectorConfig cfg, Sink sink);
    ~Collector();

    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;
    Collector(Collector&&) = delete;
    Collector& operator=(Collector&&) = delete;

    /// Take baseline readings and start the sampling thread. Idempotent.
    void start();
    /// Request the thread to stop and join it. Idempotent, safe to call from the destructor.
    void stop() noexcept;

    /**
     * Take one sample synchronously, computing rates relative to the previous call.
     * Not thread-safe with respect to a running collector thread.
     * @return std::nullopt if no source could be read at all.
     */
    [[nodiscard]] std::optional<TelemetrySample> sample_once();

    /// Number of samples where at least one source failed to read.
    [[nodiscard]] std::uint64_t read_failures() const noexcept {
        return read_failures_.load(std::memory_order_relaxed);
    }

private:
    void run();

    CollectorConfig cfg_;
    Sink sink_;
    std::chrono::nanoseconds period_;

    // Sampling state (owned by whichever thread calls sample_once()).
    std::optional<CpuTimes> prev_cpu_;
    std::optional<NetCounters> prev_net_;
    std::chrono::steady_clock::time_point prev_time_;
    std::uint64_t next_seq_{0};
    std::atomic<std::uint64_t> read_failures_{0};

    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_requested_{false};
    std::thread thread_;
};

}  // namespace etd
