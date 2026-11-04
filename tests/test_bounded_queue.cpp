#include "bounded_queue.hpp"
#include "test_common.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using namespace std::chrono_literals;

void test_fifo_and_overflow() {
    etd::BoundedQueue<int> q(3);
    CHECK(q.capacity() == 3 && q.size() == 0);
    CHECK(q.push(1) && q.push(2) && q.push(3));
    CHECK(!q.push(4));  // drops 1
    CHECK(!q.push(5));  // drops 2
    CHECK(q.overflow_count() == 2 && q.size() == 3);
    CHECK(q.pop_for(0ms) == 3);
    CHECK(q.pop_for(0ms) == 4);
    CHECK(q.pop_for(0ms) == 5);
    CHECK(!q.pop_for(1ms).has_value());  // timeout on empty
}

void test_close_drains() {
    etd::BoundedQueue<int> q(4);
    CHECK(q.push(7) && q.push(8));
    q.close();
    CHECK(!q.push(9));
    CHECK(q.pop() == 7);
    CHECK(q.pop() == 8);
    CHECK(!q.pop().has_value());
}

void test_concurrent() {
    constexpr std::uint64_t kItems = 20000;
    etd::BoundedQueue<std::uint64_t> q(64);
    std::uint64_t received = 0;
    std::uint64_t last = 0;
    bool monotonic = true;
    std::thread consumer([&] {
        while (auto v = q.pop()) {
            if (received > 0 && *v <= last) {
                monotonic = false;
            }
            last = *v;
            ++received;
        }
    });
    for (std::uint64_t i = 1; i <= kItems; ++i) {
        static_cast<void>(q.push(i));
    }
    q.close();
    consumer.join();
    CHECK(monotonic);  // drop-oldest never reorders
    CHECK(received + q.overflow_count() == kItems);
    CHECK(last == kItems);  // newest element always survives
}

void test_zero_capacity_throws() {
    bool threw = false;
    try {
        etd::BoundedQueue<int> q(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

}  // namespace

int main() {
    test_fifo_and_overflow();
    test_close_drains();
    test_concurrent();
    test_zero_capacity_throws();
    return etd_test::finish("bounded_queue");
}
