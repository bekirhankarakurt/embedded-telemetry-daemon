#include "collector.hpp"
#include "temp_dir.hpp"
#include "test_common.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream(p) << content;
}

void write_proc(const fs::path& root, std::uint64_t busy, std::uint64_t idle, std::uint64_t rx, std::uint64_t tx) {
    fs::create_directories(root / "net");
    write_file(root / "stat", "cpu  " + std::to_string(busy) + " 0 0 " + std::to_string(idle) + " 0 0 0 0\n");
    write_file(root / "meminfo", "MemTotal: 1000 kB\nMemAvailable: 250 kB\nSwapTotal: 100 kB\nSwapFree: 40 kB\n");
    write_file(root / "net" / "dev",
               "Inter-| Receive | Transmit\n face |bytes packets|bytes packets\n"
               "  eth0: " +
                   std::to_string(rx) + " 1 0 0 0 0 0 0 " + std::to_string(tx) + " 1 0 0 0 0 0 0\n");
}

void test_sample_from_fixture_root() {
    const etd_test::TempDir dir;
    write_proc(dir.path(), 100, 900, 1000, 2000);
    etd::Collector c({10.0, dir.path().string()}, [](const etd::TelemetrySample&) {});
    const auto first = c.sample_once();
    CHECK(first.has_value() && first->seq == 0);
    if (first) {
        CHECK(first->mem_total_kb == 1000 && first->mem_available_kb == 250 && first->mem_used_kb == 750);
        CHECK(first->swap_total_kb == 100 && first->swap_free_kb == 40);
        CHECK(first->net_rx_bytes == 1000 && first->net_tx_bytes == 2000);
        CHECK(first->cpu_percent == 0.0);  // no previous snapshot yet
        CHECK(first->uptime_s > 0 && first->wall_ns > 0 && first->mono_ns > 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_proc(dir.path(), 175, 925, 6000, 2000);  // 75 busy of 100 jiffies
    const auto second = c.sample_once();
    CHECK(second.has_value() && second->seq == 1);
    if (second) {
        CHECK_NEAR(second->cpu_percent, 75.0, 1e-9);
        CHECK(second->net_rx_bytes_per_s > 0.0 && second->net_tx_bytes_per_s == 0.0);
        CHECK(second->mono_ns > first->mono_ns);
    }
    CHECK(c.read_failures() == 0);
}

void test_missing_sources_are_counted() {
    const etd_test::TempDir dir;  // empty: only sysinfo() works
    etd::Collector c({1.0, dir.path().string()}, [](const etd::TelemetrySample&) {});
    const auto s = c.sample_once();
    CHECK(s.has_value() && s->mem_total_kb == 0);
    CHECK(c.read_failures() == 1);
}

void test_threaded_sampling_real_proc() {
    std::mutex m;
    std::vector<etd::TelemetrySample> got;
    etd::Collector c({100.0, "/proc"}, [&](const etd::TelemetrySample& s) {
        const std::lock_guard<std::mutex> lk(m);
        got.push_back(s);
    });
    c.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = std::chrono::steady_clock::now();
    c.stop();  // must return promptly (condvar wake-up)
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
    c.stop();  // idempotent

    const std::lock_guard<std::mutex> lk(m);
    CHECK(got.size() >= 10 && got.size() <= 40);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].seq == i);
        CHECK(got[i].cpu_percent >= 0.0 && got[i].cpu_percent <= 100.0);
        CHECK(got[i].mem_total_kb > 0 && got[i].mem_used_kb <= got[i].mem_total_kb);
    }
}

void test_invalid_config() {
    int threw = 0;
    for (const double hz : {0.0, -1.0, 1001.0}) {
        try {
            etd::Collector c({hz, "/proc"}, [](const etd::TelemetrySample&) {});
        } catch (const std::invalid_argument&) {
            ++threw;
        }
    }
    try {
        etd::Collector c({1.0, "/proc"}, nullptr);
    } catch (const std::invalid_argument&) {
        ++threw;
    }
    CHECK(threw == 4);
}

}  // namespace

int main() {
    test_sample_from_fixture_root();
    test_missing_sources_are_counted();
    test_threaded_sampling_real_proc();
    test_invalid_config();
    return etd_test::finish("collector");
}
