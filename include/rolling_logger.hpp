/**
 * @file rolling_logger.hpp
 * @brief Size-rotated JSON-lines log file and an asynchronous logger thread.
 */
#pragma once

#include "bounded_queue.hpp"
#include "telemetry_data.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace etd {

/// Logger configuration.
struct LoggerConfig {
    std::string path{"telemetry.jsonl"};  ///< Active log file; backups are path.1 .. path.N.
    std::size_t max_bytes{1024 * 1024};   ///< Rotate before a write would exceed this size.
    unsigned backups{3};                  ///< Number of rotated files kept (0 = truncate in place).
    std::size_t queue_capacity{256};      ///< Ring buffer depth between collector and writer.
};

/**
 * Append-only file with size-based rotation (path -> path.1 -> ... -> path.N).
 *
 * I/O errors (EACCES, ENOSPC, a deleted directory, ...) are counted and reported
 * on stderr once per failure streak; the file is re-opened at most once per second
 * so a transient problem heals itself without taking the daemon down.
 * Not thread-safe: owned by a single writer thread. Counters may be read from any thread.
 */
class RotatingFile {
public:
    RotatingFile(std::string path, std::size_t max_bytes, unsigned backups) noexcept;
    ~RotatingFile();

    RotatingFile(const RotatingFile&) = delete;
    RotatingFile& operator=(const RotatingFile&) = delete;
    RotatingFile(RotatingFile&&) = delete;
    RotatingFile& operator=(RotatingFile&&) = delete;

    /// Append @p data (one or more complete lines). @return true if fully written.
    bool write(std::string_view data) noexcept;

    [[nodiscard]] std::uint64_t lines_written() const noexcept { return lines_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t rotations() const noexcept { return rotations_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    bool ensure_open() noexcept;
    bool rotate() noexcept;
    void close_fd() noexcept;
    void report(const char* what, int err) noexcept;

    std::string path_;
    std::size_t max_bytes_;
    unsigned backups_;
    int fd_{-1};
    std::size_t size_{0};
    bool in_error_{false};
    std::chrono::steady_clock::time_point next_open_attempt_{};
    std::atomic<std::uint64_t> lines_{0};
    std::atomic<std::uint64_t> rotations_{0};
    std::atomic<std::uint64_t> errors_{0};
};

/**
 * Asynchronous JSON-lines logger: submit() enqueues into a bounded ring buffer
 * (never blocks the collector); a dedicated thread serializes and writes samples.
 */
class RollingLogger {
public:
    explicit RollingLogger(const LoggerConfig& cfg);
    ~RollingLogger();

    RollingLogger(const RollingLogger&) = delete;
    RollingLogger& operator=(const RollingLogger&) = delete;
    RollingLogger(RollingLogger&&) = delete;
    RollingLogger& operator=(RollingLogger&&) = delete;

    /// Start the writer thread. Idempotent.
    void start();
    /// Enqueue a sample; drops the oldest queued sample if the buffer is full.
    void submit(const TelemetrySample& s);
    /// Close the queue, drain remaining samples and join the writer. Idempotent.
    void stop() noexcept;

    [[nodiscard]] std::uint64_t queue_overflows() const noexcept { return queue_.overflow_count(); }
    [[nodiscard]] const RotatingFile& file() const noexcept { return file_; }

private:
    void run() noexcept;

    BoundedQueue<TelemetrySample> queue_;
    RotatingFile file_;
    std::thread thread_;
};

}  // namespace etd
