#!/usr/bin/env bash
# Build the selected profile through the canonical Makefile rules.
set -euo pipefail
cd -- "$(dirname -- "$0")"

if [[ -z "${PROJECT:-}" ]]; then
  echo "ERROR: set PROJECT explicitly; available profiles:" >&2
  make list-projects >&2 || true
  echo "Usage: PROJECT=<profile> [NDK_ROOT=<path>] [API=33] ./build.sh" >&2
  exit 2
fi

make_args=("PROJECT=$PROJECT" "API=${API:-33}")
ndk_root="${NDK_ROOT:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [[ -n "$ndk_root" ]]; then
  make_args+=("NDK_ROOT=$ndk_root")
else
  unset NDK_ROOT
fi

make "${make_args[@]}" preload
mkdir -p prebuilt
cp "build/$PROJECT/bin/preload.so" prebuilt/preload.so
cp "build/embed/su_daemon_aarch64_pie" prebuilt/su_daemon_aarch64_pie
sha256sum prebuilt/preload.so
echo "done: prebuilt/preload.so (PROJECT=$PROJECT, API=${API:-33})"
