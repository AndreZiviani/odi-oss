# Hardening flags for every freestanding binary in src/, in one place so the
# seven Makefiles cannot drift apart. docs/CROSS-COMPILING.md, "Hardening",
# has the whole matrix: what each flag buys on this target, and why the
# usual ones that are missing here (FORTIFY, PIE, RELRO, BIND_NOW,
# -fcf-protection) are missing.
#
# -fstack-protector-strong       a guard word below the saved registers of
#                                every function with a local array or a local
#                                whose address is taken, checked on return.
#                                The guard and the failure path are ours, in
#                                diag/src/start.S: no libc provides them.
# -fstack-clash-protection       probes every page of a frame larger than one
#                                page, so no frame can step over the guard gap
#                                below the stack.
# -ftrivial-auto-var-init=zero   locals start at zero, never at what the last
#                                call left on the stack.
# -fno-strict-overflow           signed and pointer overflow wrap, so a bounds
#                                check written as an overflow test is never
#                                optimised away.
# -Wvla                          a variable-length array is an attacker-sized
#                                stack allocation; there are none, keep it so.
# -z noexecstack                 PT_GNU_STACK without the execute bit.
HARDEN_CFLAGS := -fstack-protector-strong -fstack-clash-protection \
                 -ftrivial-auto-var-init=zero -fno-strict-overflow -Wvla
HARDEN_LDFLAGS := -Wl,-z,noexecstack

# The check every shipped binary passes before it is stripped (symbols are
# needed to find the guard): tools/check-hardening.sh.
HARDEN_CHECK = sh $(TOP)/tools/check-hardening.sh $(CROSS)
