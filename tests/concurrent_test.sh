#!/usr/bin/env bash
# Two connections open at once through one proxy, relayed independently.
#
# Connection A is held open from this shell via /dev/tcp while connection B
# runs start to finish. A proxy that served connections one at a time would
# never accept B, and ctest's timeout would fail the test. tcptap-echo serves a
# single connection and then frees the port, so B gets a second echo server.
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
echo_port="${3:?upstream port}"

# shellcheck source=tests/lib.sh
source "$(dirname "$0")/lib.sh"
trap 'exec 3>&- 2>/dev/null || true; cleanup' EXIT

# Send on A and expect exactly those bytes back, nothing of B's mixed in.
echo_on_a() {
  local reply=""
  printf '%s' "$1" >&3
  read -r -t 5 -n "${#1}" -u 3 reply || true
  [ "${reply}" = "$1" ] || fail "connection A got [${reply}], wanted [$1]"
}

start echo-a "${bin_dir}/tcptap-echo" "${echo_port}"
wait_for_line "${work}/echo-a.log" "echo listening" || fail "first upstream never came up"
start tap "${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${echo_port}"
wait_for_line "${work}/tap.log" "listening on" || fail "proxy never came up"

exec 3<>"/dev/tcp/127.0.0.1/${tap_port}"
echo_on_a "held-open-before"

start echo-b "${bin_dir}/tcptap-echo" "${echo_port}"
wait_for_line "${work}/echo-b.log" "echo listening" || fail "second upstream never came up"
# Bounded rather than left to ctest's timeout, so a serial proxy fails here
# with a reason and the cleanup still kills everything this script started.
"${bin_dir}/tcptap-send" "${tap_port}" "while-a-is-open" >"${work}/send.log" 2>&1 &
send_pid=$!
pids+=("${send_pid}")
tries=50
while kill -0 "${send_pid}" 2>/dev/null; do
  tries=$((tries - 1))
  [ "${tries}" -gt 0 ] || fail "connection B was never served while A was open"
  sleep 0.1
done
wait "${send_pid}" || fail "connection B failed while A was open"
grep -q "got 15 bytes \[while-a-is-open\]" "${work}/send.log" \
  || fail "connection B did not get exactly its own bytes back"

echo_on_a "held-open-after"
exec 3>&-

echo "PASS: two concurrent connections relayed independently"
