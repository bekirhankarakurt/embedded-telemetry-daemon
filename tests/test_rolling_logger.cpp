#include "rolling_logger.hpp"
#include "temp_dir.hpp"
#include "test_common.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::size_t count_lines(const fs::path& p) {
    std::ifstream in(p);
    std::size_t n = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++n;
    }
    return n;
}

void test_rotation() {
    const etd_test::TempDir dir;
    const fs::path log = dir.path() / "t.jsonl";
    const std::string line(99, 'x');  // 100 bytes with '\n'
    {
        etd::RotatingFile f(log.string(), 350, 2);
        for (int i = 0; i < 10; ++i) {
            CHECK(f.write(line + "\n"));
        }
        CHECK(f.lines_written() == 10);
        CHECK(f.rotations() == 3);  // 3 lines per file: 3+3+3+1
        CHECK(f.errors() == 0);
    }
    CHECK(fs::exists(log) && fs::exists(log.string() + ".1") && fs::exists(log.string() + ".2"));
    CHECK(!fs::exists(log.string() + ".3"));  // only 2 backups kept
    CHECK(fs::file_size(log) == 100);
    CHECK(fs::file_size(log.string() + ".1") == 300 && fs::file_size(log.string() + ".2") == 300);
}

void test_truncate_without_backups() {
    const etd_test::TempDir dir;
    const fs::path log = dir.path() / "t.jsonl";
    etd::RotatingFile f(log.string(), 150, 0);
    for (int i = 0; i < 5; ++i) {
        CHECK(f.write(std::string(99, 'y') + "\n"));
    }
    CHECK(fs::file_size(log) == 100);
    CHECK(!fs::exists(log.string() + ".1"));
}

void test_appends_to_existing_file() {
    const etd_test::TempDir dir;
    const fs::path log = dir.path() / "t.jsonl";
    { std::ofstream(log) << std::string(299, 'z') << '\n'; }
    etd::RotatingFile f(log.string(), 350, 1);
    CHECK(f.write(std::string(99, 'a') + "\n"));  // 300 + 100 > 350 -> rotate first
    CHECK(f.rotations() == 1 && fs::file_size(log) == 100 && fs::file_size(log.string() + ".1") == 300);
}

void test_unwritable_path_is_not_fatal() {
    const etd_test::TempDir dir;
    etd::RotatingFile f((dir.path() / "missing" / "t.jsonl").string(), 1024, 1);
    CHECK(!f.write("{}\n"));
    CHECK(!f.write("{}\n"));  // throttled retry, still no crash
    CHECK(f.errors() >= 1 && f.lines_written() == 0);
}

void test_async_logger() {
    const etd_test::TempDir dir;
    etd::LoggerConfig cfg;
    cfg.path = (dir.path() / "async.jsonl").string();
    cfg.max_bytes = 1U << 20U;
    cfg.backups = 1;
    cfg.queue_capacity = 1024;
    {
        etd::RollingLogger logger(cfg);
        logger.start();
        etd::TelemetrySample s;
        for (std::uint64_t i = 0; i < 100; ++i) {
            s.seq = i;
            logger.submit(s);
        }
        logger.stop();  // drains the queue
        CHECK(logger.file().lines_written() == 100 && logger.queue_overflows() == 0);
    }
    CHECK(count_lines(cfg.path) == 100);
    std::ifstream in(cfg.path);
    std::string first;
    std::getline(in, first);
    const auto parsed = etd::parse_json_line(first);
    CHECK(parsed.has_value() && parsed->seq == 0);
}

}  // namespace

int main() {
    test_rotation();
    test_truncate_without_backups();
    test_appends_to_existing_file();
    test_unwritable_path_is_not_fatal();
    test_async_logger();
    return etd_test::finish("rolling_logger");
}
