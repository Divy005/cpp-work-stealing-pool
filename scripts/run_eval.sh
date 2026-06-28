#!/usr/bin/env bash
# Build (Release) and run the evaluation harness. Pass-through args go to
# wsp_eval, e.g.  scripts/run_eval.sh --tasks 1000000 --runs 5
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" --target wsp_eval wsp_bench >/dev/null

echo "### microbenchmark ###"
./build/wsp_bench
echo
echo "### evaluation matrix ###"
./build/wsp_eval "$@"
