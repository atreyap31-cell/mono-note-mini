#!/usr/bin/env bash
# Build, retrying past the toolchain's own intermittent failures.
#
# Two separate faults show up here, neither in this project's code:
#
#   TypeError: expected str, bytes or os.PathLike object, not function
#     in the platform's arduino.py, shortening include paths. One entry of
#     CPPPATH arrives as a callable rather than a path. Library scan order
#     varies between runs, so it appears perhaps one run in three.
#
#   Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.
#     the interpreter itself crashing.
#
#   internal compiler error: in try_forward_edges, at cfgcleanup.cc:580
#     GCC 14.2 giving up on the large switch in loop(). The -fno-* flags in
#     platformio.ini hold it off most of the time; it is nondeterministic and
#     the same source compiles on a retry.
#
# Both are cured by running it again, which is a poor answer but the honest
# one: neither is ours to fix, and a real compile error still surfaces because
# this only retries when no "error:" line was produced.

set -uo pipefail
PIO="${PIO:-T:/pio-venv/Scripts/platformio.exe}"
LOG="$(dirname "$0")/.pio/last-build.log"
mkdir -p "$(dirname "$LOG")"

for attempt in 1 2 3 4 5; do
  "$PIO" run "$@" > "$LOG" 2>&1
  rc=$?
  if grep -q "SUCCESS" "$LOG"; then
    grep -E "RAM:|Flash:|SUCCESS" "$LOG"
    exit 0
  fi
  if grep -q "internal compiler error" "$LOG"; then
    echo "compiler ICE on attempt $attempt (not your code), retrying..." >&2
    continue
  fi
  # A real compile error is "file.cpp:12:34: error: ...". Matching the bare
  # word caught "Fatal Python error:" - the interpreter crashing, which is one
  # of the glitches this is meant to retry - and abandoned the retries every
  # time it happened.
  if grep -qE "^[^ ]+:[0-9]+:[0-9]+: (error|fatal error):" "$LOG"; then
    echo "--- compile errors (attempt $attempt) ---"
    grep -E ":[0-9]+:[0-9]+: (error|fatal error):|Error [0-9]" "$LOG" | head -20
    exit 1
  fi
  echo "toolchain glitched on attempt $attempt, retrying..." >&2
done

echo "--- gave up after 5 attempts ---" >&2
tail -20 "$LOG" >&2
exit 1
