#!/usr/bin/env bash
# A deliberately slow reader still receives every byte, in order.
#
# The client writes 1.5 MB in the background and reads the echo back one
# 64 KiB read at a time with a pause between reads. That is far more than the
# socket buffers plus the proxy's high-water mark hold, so the proxy has to
# stop reading, hold partial sends and let TCP push back on the writer. A
# dropped or reordered chunk changes the zero-padded counters that come back.
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
echo_port="${3:?upstream port}"

work="$(mktemp -d)"
pids=()
cleanup() {
  exec 3>&- 2>/dev/null || true
  # Some of these have usually exited already; kill failing on them is not an
  # error, and under set -e it would turn a passing run into a failure.
  if [ "${#pids[@]}" -gt 0 ]; then kill "${pids[@]}" 2>/dev/null || true; fi
  rm -rf "${work}"
  return 0
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  tail -n 5 "${work}"/*.log >&2
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

seq -w 1 250000 | tr -d '\n' >"${work}/payload"
size="$(wc -c <"${work}/payload" | tr -d ' ')"

"${bin_dir}/tcptap-echo" "${echo_port}" >"${work}/echo.log" 2>&1 &
pids+=($!)
wait_for "${work}/echo.log" "echo listening" || fail "upstream never came up"
"${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${echo_port}" --once \
  >"${work}/tap.log" 2>&1 &
tap_pid=$!
pids+=("${tap_pid}")
wait_for "${work}/tap.log" "listening on" || fail "proxy never came up"

exec 3<>"/dev/tcp/127.0.0.1/${tap_port}"
cat "${work}/payload" >&3 &
writer_pid=$!
pids+=("${writer_pid}")

: >"${work}/reply"
deadline=$((SECONDS + 20))
while [ "$(wc -c <"${work}/reply")" -lt "${size}" ]; do
  [ "${SECONDS}" -lt "${deadline}" ] || fail "reader stalled at $(wc -c <"${work}/reply") bytes"
  dd bs=65536 count=1 <&3 >>"${work}/reply" 2>/dev/null
  sleep 0.02
done

wait "${writer_pid}" || fail "writer failed"
exec 3>&-
wait "${tap_pid}" || fail "proxy exited non-zero"

cmp -s "${work}/payload" "${work}/reply" || fail "reply differs from what was sent"
grep -q "closed: ${size} bytes upstream, ${size} bytes downstream" "${work}/tap.log" \
  || fail "proxy byte counters disagree with what was sent"

echo "PASS: a slow reader got all ${size} bytes back in order"
