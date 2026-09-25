# Cross-toolchain for the ODI DFP-34X-2C2 (RTL9601C / RLX5281, big-endian MIPS).
#
# Debian's mips-linux-gnu targets big-endian MIPS o32 — the right endianness and
# ABI for this stick. diag is built freestanding, so the fact that this
# toolchain's libc is glibc (far too new for the stick's Linux 2.6.30.9) never
# comes into play, and no vendor toolchain is needed.
#
# linux-libc-dev-mips-cross is here only for <asm/unistd.h>: the o32 syscall
# numbers are not the generic ones, and they are worth taking from the kernel's
# own headers rather than from memory. Nothing links against it.
#
# qemu-user-static runs the results here. It emulates a full MIPS32 CPU, so it
# proves logic and syscalls but NOT instruction legality — `mul` and `clz` run
# fine under qemu and trap on the real RLX5281.
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc-mips-linux-gnu \
      binutils-mips-linux-gnu \
      linux-libc-dev-mips-cross \
      qemu-user-static \
      make file xxd python3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
