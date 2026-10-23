#!/usr/bin/env bash
# A reset must not spin the proxy once a direction is throttled.
#
# The echo fixture floods past the high-water mark and never reads back, so
# up->down throttles while down->up has nothing queued: interest on the
# upstream socket drops to zero. Killing echo then leaves the client's bytes
# it never read, turning its close into a reset instead of a FIN.
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
echo_port="${3:?upstream port}"

# shellcheck source=tests/lib.sh
source "$(dirname "$0")/lib.sh"
trap 'exec 3>&- 2>/dev/null || true; cleanup' EXIT

start echo "${bin_dir}/tcptap-echo" "${echo_port}" --burst-reset
echo_pid="${last_pid}"
wait_for_line "${work}/echo.log" "echo listening" || fail "upstream never came up"

start tap "${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${echo_port}" --once
tap_pid="${last_pid}"
wait_for_line "${work}/tap.log" "listening on" || fail "proxy never came up"

exec 3<>"/dev/tcp/127.0.0.1/${tap_port}"
printf 'hi' >&3
wait_for_line "${work}/echo.log" "echo accepted" || fail "upstream never accepted"
sleep 2 # let the burst push the up->down direction past the high-water mark

kill -KILL "${echo_pid}"
wait "${echo_pid}" 2>/dev/null || true

deadline=$((SECONDS + 10))
while kill -0 "${tap_pid}" 2>/dev/null; do
  [ "${SECONDS}" -lt "${deadline}" ] || fail "proxy spun instead of closing on the reset"
  sleep 0.1
done

echo "PASS: a reset on a throttled, unread direction closed the proxy"
