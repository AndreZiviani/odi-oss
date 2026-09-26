#!/usr/bin/env bash
#
# The action trace of /etc/init.d/rcS, against a golden per flag set.
#
# rcS runs as written, on the MIPS busybox the image ships, in a chroot under
# qemu-user, with every hardware, network and daemon step stubbed and /proc
# a directory of plain files. strace records what it does; the trace keeps
# the commands it executes (with their arguments) and every write it makes
# under /proc, in process-tree order (test/rcs_trace_canon.py has why that
# order is stable). Three flag sets: none, breadcrumbs.on and confirm-arp.
#
# A change to rcS that is meant to leave the boot alone must leave all three
# identical; one that is meant to change it shows exactly what moved.
#
#   BUSYBOX=out/busybox bash test/rcs_trace_test.sh      compare
#   UPDATE=1 bash test/rcs_trace_test.sh                 rewrite the goldens
#
# Needs Docker with ptrace allowed (--cap-add SYS_PTRACE) and a built
# busybox; so it is `make test-rcs`, not part of `make test`.
#
# What it cannot see: anything the stubs replace (network.sh, services, the
# daemons, diag and devmem output), real /proc contents and the order of
# events between processes.
set -u
cd "$(dirname "$0")/.." || exit 1
BUSYBOX=${BUSYBOX:-out/busybox}
FIX=test/fixtures
SETS="none breadcrumbs confirm-arp"

[ -f "$BUSYBOX" ] || { echo "no busybox at $BUSYBOX -- make busybox, or set BUSYBOX" >&2; exit 1; }
# The freestanding toolchain image (toolchain/images.env), not a Dockerfile
# of our own: it has qemu-mips-static already but not strace, so the
# container installs that one package itself before the inner script runs.
IMAGE=$(toolchain/image.sh diag) || exit 1

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
bb=$(cd "$(dirname "$BUSYBOX")" && pwd)/$(basename "$BUSYBOX")

docker run --rm --cap-add=SYS_PTRACE --cap-add=NET_ADMIN \
	-v "$bb:/bb/busybox:ro" -v "$PWD/rootfs:/imagesrc:ro" -v "$PWD/test:/t:ro" \
	-v "$OUT:/out" -e SETS="$SETS" "$IMAGE" \
	bash -c 'apt-get -qq update && apt-get -qq -y install strace >/dev/null && exec bash /t/rcs_trace_inner.sh' || exit 1

fail=0
for s in $SETS; do
	if [ "${UPDATE:-0}" = 1 ]; then
		cp "$OUT/$s.txt" "$FIX/rcs-trace-$s.txt"
		echo "rcs-trace-$s: written ($(grep -c . "$FIX/rcs-trace-$s.txt") actions)"
	elif diff -u "$FIX/rcs-trace-$s.txt" "$OUT/$s.txt"; then
		echo "rcs-trace-$s: identical"
	else
		echo "rcs-trace-$s: DIFFERS"
		fail=1
	fi
done
exit "$fail"
