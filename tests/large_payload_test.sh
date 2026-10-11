#!/usr/bin/env bash
# Relay a payload larger than the proxy's 4 KiB buffer.
#
# Everything else in the suite fits in one recv(), so the relay loop never has
# to go round twice for one message. This one cannot: 10000 bytes needs at
# least three reads each way, and the payload is a run of zero-padded counters,
# so a dropped, duplicated or reordered chunk changes the bytes that come back.
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
echo_port="${3:?upstream port}"

work="$(mktemp -d)"
pids=()
cleanup() {
  # Some of these have usually exited already; kill failing on them is not an
  # error, and under set -e it would turn a passing run into a failure.
  if [ "${#pids[@]}" -gt 0 ]; then kill "${pids[@]}" 2>/dev/null || true; fi
  rm -rf "${work}"
  return 0
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  tail -n 20 "${work}"/*.log >&2
  exit 1
}

wait_for() {
  local tries=100
  until grep -q "$2" "$1" 2>/dev/null; do
    tries=$((tries - 1))
    [ "${tries}" -gt 0 ] || return 1
    sleep 0.1
  done
}

message="$(seq -w 1 2500 | tr -d '\n')"
size="${#message}"
[ "${size}" -eq 10000 ] || fail "built a ${size}-byte payload, wanted 10000"

"${bin_dir}/tcptap-echo" "${echo_port}" >"${work}/echo.log" 2>&1 &
pids+=($!)
wait_for "${work}/echo.log" "echo listening" || fail "upstream never came up"
"${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${echo_port}" --once \
  >"${work}/tap.log" 2>&1 &
pids+=($!)
wait_for "${work}/tap.log" "listening on" || fail "proxy never came up"

"${bin_dir}/tcptap-send" "${tap_port}" "${message}" >"${work}/send.log" 2>&1 \
  || fail "client exited non-zero"
wait "${pids[1]}" || fail "proxy exited non-zero"

# Compared as strings, not with grep: macOS grep runs out of memory on a
# 10000-byte pattern.
reply="$(sed -n "s/^got ${size} bytes \[\(.*\)\]\$/\1/p" "${work}/send.log")"
[ "${reply}" = "${message}" ] || fail "the payload did not come back byte for byte"
grep -q "closed: ${size} bytes upstream, ${size} bytes downstream" "${work}/tap.log" \
  || fail "proxy byte counters disagree with what was sent"
chunks="$(grep -c '^c>u [0-9]* bytes' "${work}/tap.log")"
[ "${chunks}" -ge 3 ] || fail "only ${chunks} upstream chunk(s); the loop never iterated"

echo "PASS: relayed ${size} bytes each way in ${chunks} upstream chunks"
