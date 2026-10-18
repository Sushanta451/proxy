#!/usr/bin/env bash
# End-to-end: tcptap-send -> tcptap -> tcptap-echo -> tcptap -> tcptap-send.
#
# Three real processes and two real TCP connections. The proxy is the thing under
# test; the echo server and the client are fixtures.
#
# On buffering: a redirected stdout is block-buffered, so a line a live process
# has "printed" may still be sitting in its buffer. Two things follow. The
# startup handshake below greps for the "listening" lines, which the programs
# flush explicitly for this purpose. Everything else is asserted only after the
# writing process has exited and its buffer has been drained.
#
# The byte-for-byte chunk assertions assume the message arrives in one recv.
# That holds for a short payload on loopback; keep test messages well under the
# 64-byte preview limit so the proxy logs them whole.
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
echo_port="${3:?upstream port}"
message="${4:?message to relay}"

# shellcheck source=tests/lib.sh
source "$(dirname "$0")/lib.sh"

start echo "${bin_dir}/tcptap-echo" "${echo_port}"
echo_pid="${last_pid}"
wait_for_line "${work}/echo.log" "echo listening" || fail "upstream never came up"

start tap "${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${echo_port}" --once
tap_pid="${last_pid}"
wait_for_line "${work}/tap.log" "listening on" || fail "proxy never came up"

"${bin_dir}/tcptap-send" "${tap_port}" "${message}" >"${work}/send.log" 2>&1 \
  || fail "client exited non-zero"

wait "${tap_pid}" || fail "proxy exited non-zero"
wait "${echo_pid}" || fail "upstream exited non-zero"

size="${#message}"

grep -q "got ${size} bytes \[${message}\]" "${work}/send.log" \
  || fail "client did not get its bytes back"
grep -q "c>u ${size} bytes \[${message}\]" "${work}/tap.log" \
  || fail "proxy did not log the downstream-to-upstream chunk"
grep -q "u>c ${size} bytes \[${message}\]" "${work}/tap.log" \
  || fail "proxy did not log the upstream-to-downstream chunk"
grep -q "c>u eof" "${work}/tap.log" \
  || fail "proxy did not forward the client's half-close"
grep -q "closed: ${size} bytes upstream, ${size} bytes downstream" "${work}/tap.log" \
  || fail "proxy byte counters disagree with what was sent"

echo "PASS: relayed ${size} bytes each way through 127.0.0.1:${tap_port}"
