#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
preset="${1:-clang-debug}"
case "$preset" in
  clang-debug|clang-release|clang-ui|clang-asan) ;;
  *) echo 'Usage: ./build.sh [clang-debug|clang-release|clang-ui|clang-asan]' >&2; exit 2 ;;
esac
clang++ --version
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel "${PLPT_JOBS:-4}"
ctest --preset "$preset" --output-on-failure
