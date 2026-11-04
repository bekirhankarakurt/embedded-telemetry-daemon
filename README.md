# embedded-telemetry-daemon

[![CI](https://github.com/bekirhankarakurt/embedded-telemetry-daemon/actions/workflows/ci.yml/badge.svg)](https://github.com/bekirhankarakurt/embedded-telemetry-daemon/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

A lightweight Linux telemetry recorder with a live UDP downlink, written in modern C++17 with
**zero external dependencies** (STL + Linux/POSIX only).

The daemon samples CPU, memory, load and network statistics at a fixed rate, records every sample to
a size-rotated JSON-lines log, and streams it to a "ground station" as a CRC-protected binary packet
(or JSON). A bundled listener decodes the stream, validates integrity and reports packet loss and
reordering — the same pattern used for on-board health monitoring and telemetry downlinks in
embedded and aerospace systems.

## Features

- **Collector** — `/proc/stat` CPU deltas, `/proc/meminfo` (`MemAvailable`, with a fallback for old
  kernels), `sysinfo()` uptime and load averages, `/proc/net/dev` counters summed over non-loopback
  interfaces with byte rates. Counter wrap/reset is handled.
- **Deterministic timing** — dedicated thread, absolute `steady_clock` schedule (`sleep_until`
  semantics, no drift), overruns skip ticks instead of bursting, stop is immediate via a condition variable.
- **Binary wire format** — fixed 196-byte packet, network byte order, magic/version/type/length header,
  CRC-32 trailer. Serialization uses explicit byte shifts and `memcpy` (no type punning, no packed structs).
- **UDP downlink** — RAII, movable, non-blocking socket; unicast or broadcast (`SO_BROADCAST`);
  `EINTR` retried, `EAGAIN`/`ENOBUFS` counted as drops so the sampling path never blocks.
- **Rolling logger** — JSON lines, size-based rotation to N backups, bounded ring buffer between the
  collector and the disk writer with an overflow counter. Permission/disk errors are reported once,
  counted, and retried — they never kill the daemon.
- **Graceful shutdown** — async-signal-safe `SIGINT`/`SIGTERM` handling (lock-free atomic + self-pipe),
  ordered teardown that drains the log queue.
- **Ground-station listener** — auto-detects binary/JSON, verifies magic/version/length/CRC,
  detects sequence gaps, reordering and sender restarts, prints a live view.
- **Quality gates** — builds warning-free on GCC and Clang with a strict warning set and `-Werror`,
  unit tests via CTest, ASan+UBSan and TSan builds in CI.

## Architecture

```
                         +--------------------------------------------------+
                         |               embedded-telemetry-daemon          |
                         |                                                  |
  /proc/stat ----+       |  +-------------------+      sink (same thread)   |
  /proc/meminfo -+-----> |  | Collector thread  |----+---------------------+ |
  /proc/net/dev -+       |  |  steady_clock     |    |                     | |
  sysinfo() -----+       |  |  N Hz schedule    |    v                     v |
                         |  +-------------------+  UdpSender         BoundedQueue<Sample>
                         |                         (non-blocking)    (ring, drop-oldest,
                         |                            |               overflow counter)
                         |                            |                     |
                         |                            |                     v
                         |                            |          +----------------------+
                         |                            |          | Logger thread        |
                         |                            |          | JSON lines + rotation|
                         |                            |          +----------------------+
                         |  Main thread: signals (self-pipe), --duration, status, shutdown
                         +----------------------------|---------------------|-------------+
                                                      | UDP                 v
                                                      v              telemetry.jsonl(.1..N)
                                            telemetry_listener
                                            (ground station)
```

### Threading model

| Thread     | Responsibility                                               | Blocking?                       |
|------------|--------------------------------------------------------------|---------------------------------|
| main       | CLI, wiring, `poll()` on the signal self-pipe, status, shutdown | yes (only thread that may)   |
| collector  | sampling on an absolute schedule, UDP send, enqueue to logger | never on I/O (non-blocking socket, O(1) enqueue) |
| logger     | dequeue, serialize to JSON, write + rotate files             | yes (disk), isolated by the queue |

`SIGINT`/`SIGTERM` are blocked in the worker threads (mask inherited at creation) so delivery always
happens on the main thread. The handler only stores to a lock-free `std::atomic<bool>` and `write(2)`s
one byte to a non-blocking pipe. Shutdown order: stop collector → close queue → logger drains and
joins → final summary. All shared counters are atomics; the project is TSan-clean.

## Binary wire format (v1)

All multi-byte integers are **big-endian** (network byte order). `f64` fields are IEEE-754 binary64
bit patterns transmitted as `u64`. Total packet size: **196 bytes**.

| Offset | Size | Type | Field            | Notes                                      |
|-------:|-----:|------|------------------|--------------------------------------------|
| 0      | 4    | u32  | magic            | `0x544C4D59` (`"TLMY"`)                    |
| 4      | 1    | u8   | version          | `1`                                        |
| 5      | 1    | u8   | msg type         | `1` = telemetry                            |
| 6      | 2    | u16  | payload length   | `168`                                      |
| 8      | 8    | u64  | sequence number  | starts at 0 for each daemon run            |
| 16     | 8    | u64  | timestamp        | `CLOCK_MONOTONIC` [ns]                     |
| 24     | 8    | u64  | wall_ns          | `CLOCK_REALTIME` [ns since epoch]          |
| 32     | 8    | f64  | cpu_pct          | 0..100                                     |
| 40     | 40   | u64×5| mem_total_kb, mem_avail_kb, mem_used_kb, swap_total_kb, swap_free_kb | KiB |
| 80     | 8    | u64  | uptime_s         |                                            |
| 88     | 24   | f64×3| load1, load5, load15 |                                        |
| 112    | 64   | u64×8| net rx/tx bytes, rx/tx packets, rx/tx errors, rx/tx drops | non-loopback sum |
| 176    | 16   | f64×2| net_rx_Bps, net_tx_Bps | bytes/s over the last period         |
| 192    | 4    | u32  | CRC-32           | IEEE 802.3 (poly `0xEDB88320`) over bytes 0..191 |

The receiver rejects packets with a wrong magic, version, type, length or CRC.

## JSON-lines format

Used for the log file and for `--format json` on the downlink (one object per line):

```json
{"type":"telemetry","v":1,"seq":136,"mono_ns":597113617676,"wall_ns":1793818219579554105,"cpu_pct":3.412,"mem_total_kb":16477348,"mem_avail_kb":15943740,"mem_used_kb":533608,"swap_total_kb":0,"swap_free_kb":0,"uptime_s":598,"load1":0.453,"load5":0.189,"load15":0.060,"net_rx_bytes":43895731,"net_tx_bytes":7291957,"net_rx_packets":11631,"net_tx_packets":11482,"net_rx_errors":0,"net_tx_errors":0,"net_rx_drops":0,"net_tx_drops":0,"net_rx_Bps":5222.512,"net_tx_Bps":3276.800}
```

## Requirements

- Linux (uses `/proc`, `sysinfo(2)`, `pipe2(2)`, `SOCK_NONBLOCK`)
- C++17 compiler: GCC ≥ 9 or Clang ≥ 10 (tested with GCC 13 and Clang 18)
- CMake ≥ 3.16

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DETD_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

```bash
# Terminal 1 — ground station
./build/telemetry_listener --port 5000

# Terminal 2 — daemon at 10 Hz for 30 s, binary downlink, rotating log
./build/embedded-telemetry-daemon --rate-hz 10 --dest 127.0.0.1 --port 5000 --duration 30 --verbose
```

Install (binaries to `bin/`, unit to `lib/systemd/system`):

```bash
sudo cmake --install build --prefix /usr/local
```

## Command-line options

| Option               | Default           | Description                                          |
|----------------------|-------------------|------------------------------------------------------|
| `--rate-hz <hz>`     | `1`               | Sampling rate, `0 < hz <= 1000`                      |
| `--dest <ipv4>`      | `127.0.0.1`       | UDP destination (dotted quad)                        |
| `--port <port>`      | `5000`            | UDP destination port, 1..65535                       |
| `--broadcast`        | off               | Enable `SO_BROADCAST`; destination defaults to `255.255.255.255` |
| `--format <fmt>`     | `binary`          | Downlink payload: `binary` or `json`                 |
| `--log-file <path>`  | `telemetry.jsonl` | JSON-lines log file                                  |
| `--log-max-kb <kb>`  | `1024`            | Rotate before the file exceeds this size             |
| `--log-backups <n>`  | `3`               | Rotated files kept (`path.1` … `path.N`); 0 = truncate |
| `--no-log`           | —                 | Disable file logging                                 |
| `--no-udp`           | —                 | Disable the UDP downlink                             |
| `--duration <s>`     | `0`               | Stop after *s* seconds (0 = until signalled)         |
| `--verbose`          | —                 | One status line per second on stderr                 |
| `--help`, `--version`| —                 |                                                      |

Options accept both `--opt value` and `--opt=value`.

**Exit codes** (sysexits.h conventions): `0` success (including SIGINT/SIGTERM shutdown),
`64` invalid command line, `70` internal error, `71` OS resource error (socket, pipe, thread).

## Live testing

```bash
# Raw JSON stream with netcat
nc -u -l 5000 &
./build/embedded-telemetry-daemon --format json --no-log --rate-hz 2

# Inspect the binary packets on the wire (magic "TLMY" is visible at offset 0 of the UDP payload)
sudo tcpdump -i lo -X udp port 5000

# Bundled ground station: decodes, verifies CRC, reports gaps/reordering
./build/telemetry_listener --port 5000
```

Example listener output:

```
[bin ] seq=148    cpu=  2.4% mem=520/16091 MiB load=0.45/0.19/0.06 rx=5.1 KiB/s tx=3.2 KiB/s up=598s age=0.6ms
!! sequence gap: 3 packet(s) lost before seq 152
^C
summary: binary=150 json=0 invalid=0 lost=3 reordered=0 restarts=0
```

Broadcast to a LAN segment: `--broadcast --dest 192.168.1.255`.

## Sanitizers

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DETD_SANITIZE_ADDRESS=ON && cmake --build build-asan -j
ctest --test-dir build-asan

# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DETD_SANITIZE_THREAD=ON && cmake --build build-tsan -j
ctest --test-dir build-tsan
```

With Clang, the sanitizer runtimes come from `libclang-rt-<ver>-dev` on Debian/Ubuntu. On kernels with
high ASLR entropy TSan may need `sudo sysctl vm.mmap_rnd_bits=28`.

## Tests

Plain `assert`-style checks (always active, independent of `NDEBUG`) registered with CTest:

| Test             | Coverage                                                                       |
|------------------|--------------------------------------------------------------------------------|
| `parsers`        | `/proc/stat`, `/proc/meminfo` (incl. pre-3.14 fallback), `/proc/net/dev` fixtures, CPU % math, counter reset |
| `wire_format`    | CRC-32 reference vectors, binary round-trip, header byte order, every rejection path, single-bit-flip detection, JSON shape and parse-back |
| `bounded_queue`  | FIFO order, drop-oldest overflow accounting, close/drain, concurrent producer/consumer |
| `rolling_logger` | Rotation and backup count in a temp dir, truncate mode, append to existing file, unwritable path, async drain |
| `udp_sender`     | Address validation, loopback send/receive/decode, move semantics, broadcast socket |
| `collector`      | Fake `/proc` root, rate computation, missing sources, threaded sampling and prompt stop |

## systemd

```bash
sudo cmake --install build --prefix /usr/local
sudo systemctl daemon-reload
sudo systemctl enable --now embedded-telemetry-daemon
journalctl -u embedded-telemetry-daemon -f
```

The unit (`systemd/embedded-telemetry-daemon.service`) runs as a `DynamicUser` with a strict sandbox
(`ProtectSystem=strict`, no capabilities, `AF_INET` only), logs to `/var/log/embedded-telemetry/`,
uses `SIGTERM` for graceful shutdown and does not restart on configuration errors (exit code 64).

## Design trade-offs

- **UDP send on the collector thread, disk I/O on its own thread.** A non-blocking `sendto()` is
  bounded and cheap, so sending inline keeps downlink latency minimal; disk writes can stall for
  milliseconds (fsync storms, slow SD cards), so they are decoupled by the ring buffer.
- **Drop-oldest on overflow.** For telemetry the newest data is the most valuable; the producer never
  blocks and every loss is counted (`overflow`, `dropped`) rather than hidden.
- **Fixed-size binary packet with explicit serialization.** Byte-wise big-endian encoding is portable
  across endianness and ABI, and avoids `#pragma pack` and aliasing UB. The version byte allows the
  format to evolve; `static_assert`s pin the layout.
- **CRC-32 instead of relying on the UDP checksum.** The UDP checksum is optional on IPv4 and is not
  end-to-end across relays/serial bridges; an application CRC also catches serialization bugs.
- **IPv4 only, no DNS.** Keeps the daemon deterministic (no resolver blocking at start-up) and matches
  typical fixed-address embedded networks.
- **Parsers take `std::istream`.** The `/proc` parsers are pure functions, unit-tested against
  fixtures, and the collector's `/proc` root is configurable for tests.
- **Sampling rate up to 1 kHz.** Kernel counters (jiffies) update at `CONFIG_HZ`, so CPU % at very high
  rates is quantized; the cap prevents nonsensical configurations.

## License

MIT — see [LICENSE](LICENSE).
