/**
 * @file bounded_queue.hpp
 * @brief Fixed-capacity, drop-oldest MPSC ring buffer with blocking pop.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace etd {

/**
 * Bounded ring buffer decoupling a real-time producer from a slow consumer.
 *
 * push() never blocks on a full queue: the oldest element is overwritten and
 * the overflow counter incremented, so the producer's timing is unaffected by
 * consumer stalls (e.g. a slow disk). Memory is allocated once at construction.
 */
template <typename T>
class BoundedQueue {
public:
    /// @throws std::invalid_argument if capacity is zero.
    explicit BoundedQueue(std::size_t capacity) : buf_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue capacity must be > 0");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;
    ~BoundedQueue() = default;

    /// Enqueue an element. @return false if the oldest element had to be dropped (or the queue is closed).
    bool push(T value) {
        bool dropped = false;
        {
            const std::lock_guard<std::mutex> lk(mtx_);
            if (closed_) {
                return false;
            }
            if (count_ == buf_.size()) {
                head_ = (head_ + 1) % buf_.size();
                --count_;
                overflow_.fetch_add(1, std::memory_order_relaxed);
                dropped = true;
            }
            buf_[(head_ + count_) % buf_.size()] = std::move(value);
            ++count_;
        }
        cv_.notify_one();
        return !dropped;
    }

    /**
     * Wait up to @p timeout for an element.
     * @return the element, or std::nullopt on timeout or when closed and drained.
     */
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_.wait_for(lk, timeout, [this] { return count_ > 0 || closed_; }) || count_ == 0) {
            return std::nullopt;
        }
        T out = std::move(buf_[head_]);
        head_ = (head_ + 1) % buf_.size();
        --count_;
        return out;
    }

    /// Block until an element is available. @return std::nullopt once closed and drained.
    [[nodiscard]] std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return count_ > 0 || closed_; });
        if (count_ == 0) {
            return std::nullopt;
        }
        T out = std::move(buf_[head_]);
        head_ = (head_ + 1) % buf_.size();
        --count_;
        return out;
    }

    /// Reject further pushes and wake all waiters; remaining elements can still be popped.
    void close() noexcept {
        {
            const std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> lk(mtx_);
        return count_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }
    /// Number of elements discarded because the queue was full.
    [[nodiscard]] std::uint64_t overflow_count() const noexcept { return overflow_.load(std::memory_order_relaxed); }

private:
    std::vector<T> buf_;
    std::size_t head_{0};
    std::size_t count_{0};
    bool closed_{false};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<std::uint64_t> overflow_{0};
};

}  // namespace etd
