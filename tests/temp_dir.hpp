/**
 * @file temp_dir.hpp
 * @brief RAII temporary directory for tests.
 */
#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace etd_test {

class TempDir {
public:
    TempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "etd_test_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (::mkdtemp(buf.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace etd_test
