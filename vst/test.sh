#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan) of matrix_vst.cpp without ALSA (-DNO_ALSA): a fake host clock,
# the outgoing MIDI through a test hook, the grid over real HTTP on 127.0.0.1:18090 (FORCEHUB_PORT).
# Run after build.sh (needs build/params.h, build/popup.h). Prints PASSED/FAILED; exit code follows.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/b -w /b gcc:12 bash -euc '
  mkdir -p build/x86
  SAN="-fsanitize=address,undefined"
  g++ -O0 -g $SAN -std=c++17 -Wall -Wextra -Wno-unused-parameter -DNO_ALSA -fPIC -shared -Ibuild -I. \
      -o build/x86/matrix-x86.so matrix_vst.cpp -lpthread
  g++ -O0 -g $SAN -std=c++17 -o build/x86/host_test host_test.cpp -ldl -lpthread
  FORCEHUB_PORT=18090 ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/matrix-x86.so
'
