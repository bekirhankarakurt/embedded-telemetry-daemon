# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-11-04

### Added
- Collector thread sampling CPU (`/proc/stat`), memory and swap (`/proc/meminfo`), uptime and load
  averages (`sysinfo()`), and network counters with rates (`/proc/net/dev`, loopback excluded) on an
  absolute `steady_clock` schedule at up to 1 kHz.
- Binary wire format v1 (196 bytes, big-endian, magic `0x544C4D59`, CRC-32) and JSON-lines serialization.
- Non-blocking RAII UDP sender with unicast and broadcast support; `EAGAIN`/`ENOBUFS` counted as drops.
- Size-rotating JSON-lines logger behind a bounded drop-oldest ring buffer with overflow accounting;
  I/O errors are reported once per failure streak and retried.
- `embedded-telemetry-daemon` CLI (`--rate-hz`, `--dest`, `--port`, `--broadcast`, `--format`,
  `--log-file`, `--log-max-kb`, `--log-backups`, `--no-log`, `--no-udp`, `--duration`, `--verbose`)
  with validation, async-signal-safe SIGINT/SIGTERM shutdown and sysexits-style exit codes.
- `telemetry_listener` ground-station receiver verifying magic, version, length and CRC and reporting
  sequence gaps, reordering and sender restarts.
- Unit tests (CTest), loopback smoke test, ASan+UBSan and TSan build options.
- Hardened systemd unit.
- CI: GCC/Clang matrix with sanitizers, cppcheck and clang-tidy, coverage, CodeQL, Doxygen API docs and
  coverage report published to GitHub Pages, tagged releases for linux-x86_64 and linux-aarch64.

[Unreleased]: https://github.com/bekirhankarakurt/embedded-telemetry-daemon/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/bekirhankarakurt/embedded-telemetry-daemon/releases/tag/v1.0.0
