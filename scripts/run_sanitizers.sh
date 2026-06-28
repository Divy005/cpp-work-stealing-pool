#!/usr/bin/env bash
# Build and run the test suite under ThreadSanitizer and AddressSanitizer+UBSan.
# These are required gates: both must be clean before a phase is committed.
set -euo pipefail
cd "$(dirname "$0")/.."

run_gate() {
  local name="$1" san="$2" dir="build-${3}"
  echo "==================================================================="
  echo " $name  (-DWSP_SANITIZER=$san)"
  echo "==================================================================="
  cmake -S . -B "$dir" -DWSP_SANITIZER="$san" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
  cmake --build "$dir" -j"$(nproc)" --target wsp_tests >/dev/null
  "./$dir/wsp_tests"
  echo "[$name] PASSED"
  echo
}

# halt on the first race/error so failures are obvious.
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"
export ASAN_OPTIONS="detect_leaks=1 abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1 halt_on_error=1"

run_gate "ThreadSanitizer"          "thread"             tsan
run_gate "AddressSanitizer + UBSan" "address,undefined"  asan

echo "All sanitizer gates passed."
