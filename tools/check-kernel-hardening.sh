#!/usr/bin/env bash
#
# The kernel hardening choices of docs/KERNEL.md ("Hardening"), checked so
# that none of them goes away without someone deciding it should.
#
#     tools/check-kernel-hardening.sh               # the seed, kernel/618/config
#     tools/check-kernel-hardening.sh <.config>     # a completed .config
#
# With no argument (make lint, so every pull request) each line of REQUIRED
# below must be in the seed exactly as written: the choice is stated there,
# next to its reason. With a .config (kernel/build.sh runs this right after
# olddefconfig, before compiling) each option must have come out as chosen.
# That second check is the one that matters: olddefconfig turns a symbol off
# without a word when one of its dependencies goes away, and the seed line
# that asked for it stays where it was. A symbol required off may also be
# missing from a .config, which is how olddefconfig leaves one whose
# dependencies are not met.
#
# CMDLINE lists words the built-in command line must carry (the sysctl.*
# settings): checked on CONFIG_CMDLINE, in the seed and in a .config alike.
#
# To change a choice, change it here, in the seed and in docs/KERNEL.md
# together.
set -eu

# On: what the hardening section of docs/KERNEL.md enables, plus the two that
# were already on by default before it existed (STACKPROTECTOR_STRONG,
# INIT_STACK_ALL_ZERO), now stated in the seed.
# Off: interfaces that would widen what a local process can reach in the
# kernel, kept off by the seed today for size or for want of a user.
REQUIRED='
CONFIG_STACKPROTECTOR=y
CONFIG_STACKPROTECTOR_STRONG=y
CONFIG_INIT_STACK_ALL_ZERO=y
CONFIG_FORTIFY_SOURCE=y
CONFIG_HARDENED_USERCOPY=y
CONFIG_HARDENED_USERCOPY_DEFAULT_ON=y
CONFIG_INIT_ON_ALLOC_DEFAULT_ON=y
CONFIG_INIT_ON_FREE_DEFAULT_ON=y
CONFIG_LIST_HARDENED=y
CONFIG_BUG_ON_DATA_CORRUPTION=y
CONFIG_SCHED_STACK_END_CHECK=y
CONFIG_SECURITY_DMESG_RESTRICT=y
CONFIG_ARCH_MMAP_RND_BITS=15
CONFIG_DEFAULT_MMAP_MIN_ADDR=65536
# CONFIG_COMPAT_BRK is not set
# CONFIG_MODULES is not set
# CONFIG_IO_URING is not set
# CONFIG_INET_DIAG is not set
# CONFIG_KALLSYMS is not set
# CONFIG_BPF_SYSCALL is not set
# CONFIG_PERF_EVENTS is not set
# CONFIG_USERFAULTFD is not set
# CONFIG_KEXEC is not set
# CONFIG_MAGIC_SYSRQ is not set
# CONFIG_PROC_KCORE is not set
# CONFIG_DEBUG_FS is not set
# CONFIG_LEGACY_TIOCSTI is not set
# CONFIG_LDISC_AUTOLOAD is not set
'

CMDLINE='
sysctl.kernel.kptr_restrict=2
sysctl.fs.protected_symlinks=1
sysctl.fs.protected_hardlinks=1
sysctl.fs.protected_fifos=1
sysctl.fs.protected_regular=1
'

if [ $# -eq 0 ]; then
	file=$(cd "$(dirname "$0")/.." && pwd)/kernel/618/config
	mode=seed
else
	file=$1
	mode=config
fi
[ -r "$file" ] || { echo "check-kernel-hardening: cannot read $file" >&2; exit 1; }

status=0
fail() {
	echo "check-kernel-hardening: $file: $*" >&2
	status=1
}

while IFS= read -r want; do
	[ -n "$want" ] || continue
	if grep -qxF -- "$want" "$file"; then
		continue
	fi
	case $want in
	'# CONFIG_'*' is not set')
		sym=${want#\# }; sym=${sym%% *}
		# A .config may leave out an off symbol altogether; it must not
		# have it set to anything.
		if [ "$mode" = config ] && ! grep -q "^$sym=" "$file"; then
			continue
		fi
		fail "wants \"$want\", has \"$(grep -m1 "^$sym=" "$file" || echo none)\"" ;;
	*)
		sym=${want%%=*}
		got=$(grep -m1 -E "^$sym=|^# $sym is not set" "$file" || true)
		fail "wants \"$want\", has \"${got:-nothing}\"" ;;
	esac
done <<EOF
$REQUIRED
EOF

cmdline=$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' "$file" | tail -n 1)
[ -n "$cmdline" ] || fail "no CONFIG_CMDLINE"
while IFS= read -r word; do
	[ -n "$word" ] || continue
	case " $cmdline " in
	*" $word "*) ;;
	*) fail "CONFIG_CMDLINE lacks $word" ;;
	esac
done <<EOF
$CMDLINE
EOF

[ "$status" = 0 ] && echo "kernel hardening options ($mode): all present"
exit "$status"
