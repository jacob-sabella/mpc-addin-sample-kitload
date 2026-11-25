#!/usr/bin/env bash
# build/aarch64/mpc_sample_kitload.so with addin.manifest and the default settings. Uses the aarch64-linux-gnu cross
# compiler when present, else Docker with QEMU for arm64.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/aarch64
VER=$(cat VERSION)
F="-std=gnu11 -O2 -Wall -Wextra -Werror -mcpu=cortex-a55 -fPIC -fvisibility=hidden -DADDIN_VERSION=\"$VER\""
OUT=build/aarch64/mpc_sample_kitload.so
build() {  # $1 = compiler prefix
  ${1}gcc $F -mgeneral-regs-only -c -o build/aarch64/hooks.o src/hooks.c
  ${1}gcc $F -c -o build/aarch64/page.o src/page.c
  ${1}gcc $F -c -o build/aarch64/addin.o src/addin.c
  ${1}gcc $F -c -o build/aarch64/kit.o src/kit.c
  ${1}gcc -shared -fvisibility=hidden -Wl,-z,defs -Wl,--as-needed -o $OUT build/aarch64/addin.o build/aarch64/page.o build/aarch64/kit.o build/aarch64/hooks.o -ldl -lpthread
  ${1}strip --strip-unneeded $OUT
  rm -f build/aarch64/*.o
}
if command -v aarch64-linux-gnu-gcc >/dev/null && [ -z "${DOCKER:-}" ]; then
  build aarch64-linux-gnu-; OBJDUMP=aarch64-linux-gnu-objdump
else
  docker run --rm --platform linux/arm64 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b arm64v8/gcc:12-bookworm env DOCKER=1 bash build-aarch64.sh
  exit 0
fi
max=$($OBJDUMP -T $OUT | grep -o "GLIBC_[0-9.]*" | sort -t. -k2 -n -u | tail -n 1)
echo "highest glibc symbol version: $max"
case "$max" in GLIBC_2.[0-9]|GLIBC_2.[123][0-9]) ;; *) echo "too new for the MPC Sample (glibc 2.39): $max" >&2; exit 1 ;; esac
cp -f mpc_sample_kitload.conf addin.manifest build/aarch64/
ls -l $OUT
