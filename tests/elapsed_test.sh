#!/usr/bin/env bash
# tcptap-send reports how long its exchange took, in milliseconds.
#
# Timing tests should read this line rather than wrapping the client with
# `date`: GNU `date +%s%3N` and BSD `date` don't agree on sub-second output,
# but `elapsed` comes from the same steady_clock on every platform.
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
echo_port="${3:?upstream port}"

# shellcheck source=tests/lib.sh
source "$(dirname "$0")/lib.sh"

start echo "${bin_dir}/tcptap-echo" "${echo_port}"
echo_pid="${last_pid}"
wait_for_line "${work}/echo.log" "echo listening" || fail "upstream never came up"

start tap "${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${echo_port}" --once
tap_pid="${last_pid}"
wait_for_line "${work}/tap.log" "listening on" || fail "proxy never came up"

"${bin_dir}/tcptap-send" "${tap_port}" "time-me" >"${work}/send.log" 2>&1 \
  || fail "client exited non-zero"

wait "${tap_pid}" || fail "proxy exited non-zero"
wait "${echo_pid}" || fail "upstream exited non-zero"

elapsed="$(sed -n 's/^elapsed \([0-9]\{1,\}\) ms$/\1/p' "${work}/send.log")"
[ -n "${elapsed}" ] || fail "client did not report an elapsed time"
[ "${elapsed}" -lt 5000 ] || fail "elapsed ${elapsed} ms is implausible for loopback"

echo "PASS: tcptap-send reported ${elapsed} ms elapsed"
