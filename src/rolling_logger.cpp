#include "rolling_logger.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

namespace etd {

RotatingFile::RotatingFile(std::string path, std::size_t max_bytes, unsigned backups) noexcept
    : path_(std::move(path)), max_bytes_(max_bytes > 0 ? max_bytes : 1), backups_(backups) {}

RotatingFile::~RotatingFile() {
    close_fd();
}

void RotatingFile::close_fd() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void RotatingFile::report(const char* what, int err) noexcept {
    errors_.fetch_add(1, std::memory_order_relaxed);
    if (!in_error_) {
        // Report once per failure streak to avoid flooding the journal.
        std::fprintf(stderr, "logger: %s '%s': %s (will retry)\n", what, path_.c_str(), std::strerror(err));
        in_error_ = true;
    }
}

bool RotatingFile::ensure_open() noexcept {
    if (fd_ >= 0) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_open_attempt_) {
        return false;
    }
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        report("cannot open", errno);
        next_open_attempt_ = now + std::chrono::seconds(1);
        return false;
    }
    struct stat st {};
    size_ = (::fstat(fd_, &st) == 0 && st.st_size > 0) ? static_cast<std::size_t>(st.st_size) : 0U;
    return true;
}

bool RotatingFile::rotate() noexcept {
    close_fd();
    if (backups_ == 0) {
        fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            report("cannot truncate", errno);
            return false;
        }
    } else {
        // Shift path.(N-1) -> path.N ... path.1 -> path.2, then path -> path.1.
        for (unsigned i = backups_; i > 1; --i) {
            const std::string from = path_ + "." + std::to_string(i - 1);
            const std::string to = path_ + "." + std::to_string(i);
            if (std::rename(from.c_str(), to.c_str()) != 0 && errno != ENOENT) {
                report("cannot rotate backup of", errno);
            }
        }
        const std::string first = path_ + ".1";
        if (std::rename(path_.c_str(), first.c_str()) != 0 && errno != ENOENT) {
            report("cannot rotate", errno);
            return false;
        }
        if (!ensure_open()) {
            return false;
        }
    }
    size_ = 0;
    rotations_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RotatingFile::write(std::string_view data) noexcept {
    if (!ensure_open()) {
        return false;
    }
    if (size_ > 0 && size_ + data.size() > max_bytes_ && !rotate()) {
        return false;
    }
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            report("write failed on", errno);
            close_fd();  // re-open (and re-stat) on the next attempt
            return false;
        }
        const auto written = static_cast<std::size_t>(n);
        p += written;
        left -= written;
        size_ += written;
    }
    in_error_ = false;
    lines_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

RollingLogger::RollingLogger(const LoggerConfig& cfg)
    : queue_(cfg.queue_capacity), file_(cfg.path, cfg.max_bytes, cfg.backups) {}

RollingLogger::~RollingLogger() {
    stop();
}

void RollingLogger::start() {
    if (!thread_.joinable()) {
        thread_ = std::thread(&RollingLogger::run, this);
    }
}

void RollingLogger::submit(const TelemetrySample& s) {
    static_cast<void>(queue_.push(s));
}

void RollingLogger::stop() noexcept {
    queue_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RollingLogger::run() noexcept {
    try {
        while (auto s = queue_.pop()) {
            static_cast<void>(file_.write(to_json_line(*s)));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "logger: writer thread stopped: %s\n", e.what());
    }
}

}  // namespace etd
