#!/usr/bin/env bash
# The fault RNG's seed is logged on every run, given or drawn, and a malformed
# --seed is refused before the proxy starts listening.
#
# A fault that fires at random is only debuggable if the run can be replayed,
# and it can only be replayed if the seed is in the log even when nobody chose
# one. No connection is needed: the seed line comes before "listening on".
set -euo pipefail

bin_dir="${1:?binary directory}"
tap_port="${2:?proxy port}"
upstream_port="${3:?upstream port, never dialled}"

work="$(mktemp -d)"
tap_pid=""
cleanup() {
  if [ -n "${tap_pid}" ]; then kill "${tap_pid}" 2>/dev/null || true; fi
  rm -rf "${work}"
  return 0
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  cat "${work}"/*.log >&2 2>/dev/null || true
  exit 1
}

# Start the proxy with the given extra flags, wait until it listens, stop it.
seed_line() {
  "${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${upstream_port}" "$@" \
    >"${work}/tap.log" 2>&1 &
  tap_pid=$!
  local tries=100
  until grep -q "listening on" "${work}/tap.log" 2>/dev/null; do
    tries=$((tries - 1))
    [ "${tries}" -gt 0 ] || fail "proxy never came up with: $*"
    sleep 0.1
  done
  kill "${tap_pid}" 2>/dev/null || true
  wait "${tap_pid}" 2>/dev/null || true
  tap_pid=""
  grep '^seed ' "${work}/tap.log" || true
}

[ "$(seed_line --seed 42)" = "seed 42" ] || fail "--seed 42 was not logged as given"
[ "$(seed_line --seed 4294967295)" = "seed 4294967295" ] || fail "the largest seed was refused"
seed_line | grep -qE '^seed [0-9]+ \(random\)$' || fail "a drawn seed was not logged"

for bad in -1 4294967296 12abc ""; do
  if "${bin_dir}/tcptap" --listen "${tap_port}" --upstream "127.0.0.1:${upstream_port}" \
    --seed "${bad}" >"${work}/bad.log" 2>&1; then
    fail "--seed '${bad}' was accepted"
  fi
  grep -q "error: --seed" "${work}/bad.log" || fail "--seed '${bad}' failed without saying why"
done

echo "PASS: seeds are logged when given and when drawn, and bad ones are refused"
