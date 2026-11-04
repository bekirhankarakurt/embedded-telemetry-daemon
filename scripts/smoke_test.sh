#!/usr/bin/env bash
# End-to-end loopback smoke test: daemon -> UDP -> listener, with log rotation.
# Usage: scripts/smoke_test.sh <build-dir> [port]
set -euo pipefail

build_dir=$(cd "${1:?usage: $0 <build-dir> [port]}" && pwd)
port=${2:-5055}
work=$(mktemp -d)
listener_pid=""
cleanup() {
    if [ -n "$listener_pid" ]; then kill "$listener_pid" 2>/dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT
cd "$work"

"$build_dir/telemetry_listener" --port "$port" > listener.log 2> listener.err &
listener_pid=$!
sleep 0.5

# Small log limit on purpose: exercises rotation (old backups are deleted by design).
"$build_dir/embedded-telemetry-daemon" --rate-hz 50 --port "$port" --duration 3 --verbose \
    --log-file t.jsonl --log-max-kb 8 --log-backups 2 2> daemon.log
sleep 0.2
kill -INT "$listener_pid"
wait "$listener_pid"
listener_pid=""

tail -n 3 listener.log
received=$(grep -c '^\[bin' listener.log)
logged=$(sed -n 's/.*shutdown (duration):.* log lines=\([0-9]*\).*/\1/p' daemon.log)
echo "received=$received logged=$logged"

grep -Eq 'binary=[1-9][0-9]* json=0 invalid=0 lost=0 reordered=0' listener.log
test "$received" -gt 0
test "$logged" -eq "$received"
grep -Eq 'shutdown \(duration\):.* err=0 \| log .* err=0 overflow=0' daemon.log
test -f t.jsonl && test -f t.jsonl.1 && test -f t.jsonl.2 && test ! -e t.jsonl.3

# Exit-code and option-validation paths of the daemon.
set +e
"$build_dir/embedded-telemetry-daemon" --rate-hz 0 2> /dev/null;           test $? -eq 64 || exit 1
"$build_dir/embedded-telemetry-daemon" --no-log --no-udp 2> /dev/null;     test $? -eq 64 || exit 1
"$build_dir/embedded-telemetry-daemon" --bogus 2> /dev/null;               test $? -eq 64 || exit 1
"$build_dir/embedded-telemetry-daemon" --help > /dev/null;                 test $? -eq 0 || exit 1
"$build_dir/embedded-telemetry-daemon" --version > /dev/null;              test $? -eq 0 || exit 1
"$build_dir/embedded-telemetry-daemon" --format=json --port=$((port + 1)) --no-log --rate-hz=20 --duration=0.5 \
    2> /dev/null;                                                          test $? -eq 0 || exit 1
echo "smoke test passed"
