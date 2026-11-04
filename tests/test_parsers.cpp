#include "collector.hpp"
#include "test_common.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::ifstream fixture(const std::string& name) {
    return std::ifstream(std::string(ETD_FIXTURE_DIR) + "/" + name);
}

void test_proc_stat() {
    auto f = fixture("proc_stat.txt");
    const auto t = etd::parse_proc_stat(f);
    CHECK(t.has_value());
    if (t) {
        CHECK(t->user == 4705 && t->nice == 356 && t->system == 584 && t->idle == 3699176);
        CHECK(t->iowait == 23060 && t->irq == 0 && t->softirq == 277 && t->steal == 12);
        CHECK(t->total() == 4705U + 356U + 584U + 3699176U + 23060U + 277U + 12U);
        CHECK(t->idle_total() == 3699176U + 23060U);
    }
    // Old kernels expose only four columns.
    std::istringstream old("cpu  10 20 30 40\n");
    const auto o = etd::parse_proc_stat(old);
    CHECK(o.has_value() && o->total() == 100 && o->iowait == 0);

    std::istringstream missing("cpu0 1 2 3 4\nintr 5\n");
    CHECK(!etd::parse_proc_stat(missing).has_value());
    std::istringstream garbage("cpu  a b c d\n");
    CHECK(!etd::parse_proc_stat(garbage).has_value());
}

void test_cpu_usage() {
    const etd::CpuTimes a{100, 0, 100, 800, 0, 0, 0, 0};
    const etd::CpuTimes b{150, 0, 150, 900, 0, 0, 0, 0};  // 100 busy of 200 total
    CHECK_NEAR(etd::cpu_usage_percent(a, b), 50.0, 1e-9);
    CHECK_NEAR(etd::cpu_usage_percent(a, a), 0.0, 1e-12);
    CHECK_NEAR(etd::cpu_usage_percent(b, a), 0.0, 1e-12);  // counter reset must not go negative
    static_assert(etd::counter_delta(5, 7) == 2);
    static_assert(etd::counter_delta(7, 5) == 0);
}

void test_meminfo() {
    auto f = fixture("meminfo.txt");
    const auto m = etd::parse_meminfo(f);
    CHECK(m.has_value());
    if (m) {
        CHECK(m->total_kb == 16384000 && m->available_kb == 8192000);
        CHECK(m->swap_total_kb == 2097152 && m->swap_free_kb == 1048576);
    }
    auto legacy = fixture("meminfo_legacy.txt");
    const auto l = etd::parse_meminfo(legacy);
    CHECK(l.has_value() && l->available_kb == 100000U + 50000U + 250000U);

    std::istringstream empty("");
    CHECK(!etd::parse_meminfo(empty).has_value());
}

void test_net_dev() {
    auto f = fixture("net_dev.txt");
    const auto n = etd::parse_net_dev(f);
    CHECK(n.has_value());
    if (n) {  // lo and the malformed line are excluded
        CHECK(n->rx_bytes == 1002000 && n->rx_packets == 1020 && n->rx_errors == 1 && n->rx_drops == 3);
        CHECK(n->tx_bytes == 503000 && n->tx_packets == 830 && n->tx_errors == 3 && n->tx_drops == 4);
    }
    std::istringstream no_header("eth0: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n");
    CHECK(!etd::parse_net_dev(no_header).has_value());
}

}  // namespace

int main() {
    test_proc_stat();
    test_cpu_usage();
    test_meminfo();
    test_net_dev();
    return etd_test::finish("parsers");
}
