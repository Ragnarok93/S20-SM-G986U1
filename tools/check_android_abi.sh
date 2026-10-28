#!/usr/bin/env bash
# Small, non-project Android NDK ABI smoke test. Does not build project payloads.
set -euo pipefail

API="${API:-33}"
if [[ ! "$API" =~ ^[0-9]+$ ]]; then
  echo "ERROR: API must be a numeric Android API level" >&2
  exit 2
fi

NDK_ROOT="${NDK_ROOT:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [[ -z "$NDK_ROOT" ]]; then
  for candidate in \
    "$HOME/ndk/android-ndk-r27c" \
    "$HOME/Android/Sdk/ndk/27.2.12479018" \
    "$HOME/android-ndk-cache/android-ndk-r29"; do
    if [[ -x "$candidate/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${API}-clang" ]]; then
      NDK_ROOT="$candidate"
      break
    fi
  done
fi

if [[ -z "$NDK_ROOT" ]]; then
  echo "ERROR: set NDK_ROOT to a Linux-hosted Android NDK" >&2
  exit 2
fi

TC="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/aarch64-linux-android${API}-clang"
if [[ ! -x "$CC" ]]; then
  echo "ERROR: compiler not found: $CC" >&2
  exit 2
fi
if ! command -v readelf >/dev/null 2>&1; then
  echo "ERROR: readelf is required (install binutils)" >&2
  exit 2
fi

OUT="$(mktemp -d "${TMPDIR:-/tmp}/android-abi-check.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cat > "$OUT/abi_probe.c" <<'EOF'
int main(void) { return 0; }
EOF
cat > "$OUT/shared_probe.c" <<'EOF'
int samsung_abi_probe(void) { return 42; }
EOF

echo "== Android NDK ABI smoke test =="
echo "NDK_ROOT=$NDK_ROOT"
echo "API=$API"
"$CC" --version | sed -n '1,2p'
target="$("$CC" -dumpmachine)"
echo "target=$target"
if [[ "$target" != *"aarch64"* || "$target" != *"android$API"* ]]; then
  echo "ERROR: compiler target does not match AArch64 Android API $API" >&2
  exit 1
fi

"$CC" -Wall -Wextra -Werror -fPIE -pie "$OUT/abi_probe.c" -o "$OUT/abi_probe"
"$CC" -Wall -Wextra -Werror -fPIC -shared "$OUT/shared_probe.c" -o "$OUT/libabi_probe.so"

exe_header="$(readelf -hW "$OUT/abi_probe")"
so_header="$(readelf -hW "$OUT/libabi_probe.so")"
if ! grep -q 'Class:.*ELF64' <<<"$exe_header" ||
   ! grep -q 'Machine:.*AArch64' <<<"$exe_header" ||
   ! grep -q 'Type:.*DYN' <<<"$exe_header"; then
  echo "ERROR: executable is not an AArch64 ELF64 PIE" >&2
  exit 1
fi
if ! readelf -lW "$OUT/abi_probe" | grep -Fq '/system/bin/linker64'; then
  echo "ERROR: executable does not request Android's linker64" >&2
  exit 1
fi
if ! grep -q 'Class:.*ELF64' <<<"$so_header" ||
   ! grep -q 'Machine:.*AArch64' <<<"$so_header" ||
   ! grep -q 'Type:.*DYN' <<<"$so_header"; then
  echo "ERROR: shared library is not an AArch64 ELF64 shared object" >&2
  exit 1
fi

echo "PASS: AArch64 Android API $API PIE and shared-library smoke tests"
echo "NOTE: this verifies toolchain/ELF basics only, not project runtime compatibility."
