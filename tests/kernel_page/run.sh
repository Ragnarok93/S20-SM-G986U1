#!/usr/bin/env bash
# Host-side checks for the kernel-page experiment.  No device, no NDK.
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host
cc -std=c11 -Wall -Wextra -Werror -I src \
  src/kernel_page/gate.c tests/kernel_page/gate_test.c \
  -o build/host/gate_test
cc -std=c11 -Wall -Wextra -Werror -I src \
  src/kernel_page/run_state.c tests/kernel_page/state_test.c \
  -o build/host/state_test
cc -std=c11 -Wall -Wextra -Werror -I src \
  src/kernel_page/address_cache.c tests/kernel_page/cache_test.c \
  -o build/host/cache_test
./build/host/gate_test
./build/host/state_test
./build/host/cache_test

# A shared library links happily with unresolved kernel_page symbols, and the
# device then refuses to load it.  Catch that here.
SO=build/samsung-g986u1-4.19.113/bin/preload.so
if [ -f "$SO" ]; then
  undef=$(nm -D "$SO" | awk '$1 == "U" && $2 ~ /^kp_/ { print $2 }')
  if [ -n "$undef" ]; then
    echo "FAIL undefined kernel_page symbols in $SO:"
    echo "$undef"
    exit 1
  fi
  echo "ok   no undefined kernel_page symbols"
fi
