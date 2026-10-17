#!/usr/bin/env bash
# Shared helper for the end-to-end tests. A test sources this, then uses:
#   start NAME CMD...        background CMD, log to "${work}/NAME.log"
#   wait_for_line FILE PAT   poll FILE for PAT, ~10s timeout
#   fail MSG...              print MSG, dump every log, exit 1
#   "${last_pid}"            the pid `start` most recently backgrounded
#
# The EXIT trap below kills every pid `start` recorded and removes the work
# dir, so a sourcing script needs no trap of its own.
set -euo pipefail

work="$(mktemp -d)"
pids=()
last_pid=""

cleanup() {
  if [ "${#pids[@]}" -gt 0 ]; then kill "${pids[@]}" 2>/dev/null || true; fi
  rm -rf "${work}"
  return 0
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  cat "${work}"/*.log >&2 2>/dev/null || true
  exit 1
}

wait_for_line() {
  local file="$1" pattern="$2" tries=100
  until grep -q "${pattern}" "${file}" 2>/dev/null; do
    tries=$((tries - 1))
    [ "${tries}" -gt 0 ] || return 1
    sleep 0.1
  done
}

start() {
  local name="$1"
  shift
  "$@" >"${work}/${name}.log" 2>&1 &
  # macOS's bash has no negative array indices, so a caller that needs the
  # pid of the process it just started reads it from here, not from
  # "${pids[-1]}".
  last_pid="$!"
  pids+=("${last_pid}")
}
