#!/usr/bin/env bash
# --delay holds a chunk roughly that long before relaying it, and only in the
# direction --direction selects; the other direction's held chunks stay due
# immediately, same as before --delay existed.
#
# Both cases round-trip a short message through the echo fixture and read the
# elapsed time tcptap-send reports. The bounds are loose enough for CI jitter
# but tight enough to fail if the delay were skipped (too low) or applied to
# both directions instead of the selected one (too high, near double).
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
echo_port="${3:?upstream port}"

# shellcheck source=tests/lib.sh
source "$(dirname "$0")/lib.sh"

delay_ms=400
low=$((delay_ms * 3 / 4))
high=$((delay_ms * 7 / 4))

# Round-trips a message through the proxy with --delay and --direction set to
# $1, and prints the elapsed ms tcptap-send reported.
elapsed_for() {
  start echo "${bin_dir}/tcptap-echo" "${echo_port}"
  echo_pid="${last_pid}"
  wait_for_line "${work}/echo.log" "echo listening" || fail "upstream never came up"

  start tap "${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${echo_port}" \
    --once --delay "${delay_ms}" --direction "$1"
  tap_pid="${last_pid}"
  wait_for_line "${work}/tap.log" "listening on" || fail "proxy never came up for --direction $1"

  "${bin_dir}/tcptap-send" "${tap_port}" "delay-me" >"${work}/send.log" 2>&1 \
    || fail "client exited non-zero for --direction $1"
  wait "${tap_pid}" || fail "proxy exited non-zero for --direction $1"
  wait "${echo_pid}" || fail "upstream exited non-zero for --direction $1"

  sed -n 's/^elapsed \([0-9]\{1,\}\) ms$/\1/p' "${work}/send.log"
}

for direction in "c>u" "u>c"; do
  elapsed="$(elapsed_for "${direction}")"
  [ -n "${elapsed}" ] || fail "--direction ${direction} never reported an elapsed time"
  [ "${elapsed}" -ge "${low}" ] \
    || fail "--direction ${direction} elapsed ${elapsed} ms, under ${low}: delay was not applied"
  [ "${elapsed}" -le "${high}" ] \
    || fail "--direction ${direction} elapsed ${elapsed} ms, over ${high}: delay leaked into the other direction"
done

echo "PASS: --delay held the selected direction ~${delay_ms} ms and left the other direction undelayed"
