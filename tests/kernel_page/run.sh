#!/usr/bin/env bash
# Host-side checks for the kernel-page experiment.  No device, no NDK.
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host
cc -std=c11 -Wall -Wextra -Werror -I src \
  src/kernel_page/gate.c tests/kernel_page/gate_test.c \
  -o build/host/gate_test
./build/host/gate_test
