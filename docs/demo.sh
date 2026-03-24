#!/usr/bin/env bash
# Scripted terminal session used to record docs/demo.gif (asciinema + agg).
# Everything printed below the typed commands is real program output.
set -u
BIN=${1:-./build}
type_cmd() { printf '\033[1;32m$\033[0m '; for ((i = 0; i < ${#1}; i++)); do printf '%s' "${1:i:1}"; sleep 0.03; done; printf '\n'; sleep 0.4; }
cd "$(mktemp -d)"

type_cmd "telemetry_listener --port 5000 &"
"$BIN/telemetry_listener" --port 5000 2>&1 &
listener=$!
sleep 0.8
type_cmd "embedded-telemetry-daemon --rate-hz 2 --duration 4 --verbose"
"$BIN/embedded-telemetry-daemon" --rate-hz 2 --duration 4 --verbose 2>&1
sleep 0.6
type_cmd "kill -INT %1"
kill -INT "$listener"
wait "$listener" 2>/dev/null
sleep 0.6
type_cmd "head -n 1 telemetry.jsonl | cut -c1-150"
head -n 1 telemetry.jsonl | cut -c1-150
sleep 2
