#!/bin/sh
# Builds the script-class test guests.
#
# -nostdlib is the point, not an optimization. These guests call
# nothing but src/sandbox/zone_abi.h, so a libc would only contribute
# its own startup syscalls -- which the host does not implement and
# should not.
#
# The toolchain is the Bootlin riscv64 glibc one installed by
# ci-local/Containerfile at /opt/riscv64-toolchain. glibc rather than
# musl per the branch decision; with -nostdlib neither libc is linked,
# so the choice only picks the compiler driver.
set -eu

CC="${CC:-/opt/riscv64-toolchain/bin/riscv64-buildroot-linux-gnu-gcc}"
OUT="${OUT:-$(dirname "$0")}"
SRC="$(dirname "$0")"

set -x
"$CC" \
	-march=rv64gc -mabi=lp64d \
	-static -nostdlib -nostartfiles -ffreestanding \
	-O2 -Wall -Wextra \
	-o "$OUT/kv_smoke.elf" "$SRC/kv_smoke.c"
