#!/bin/sh
#
# Run make targets side by side and report them, for `make test`.
#
#   tools/test-legs.sh <jobs> <make-target>...
#
# Each target is one leg: `make <target>` with its output kept in
# out/test-logs/<target>.log. A leg prints one line when it ends, so the output
# of parallel legs is never interleaved; the full log of a failed leg is
# printed at the end, in the order the targets were given. The exit status is
# non-zero when any leg failed. At most <jobs> legs run at once.
#
# Why this and not `make -j`: the make 3.81 that macOS ships hangs at the end
# of a -j run for as long as a process some test started in the background is
# still alive (a stub daemon of a host test did exactly that), and has no
# --output-sync. xargs -P needs neither and waits only for the legs.
#
# POSIX sh and xargs -P, as macOS and Linux both have them. The legs are run
# with $MAKE (default make).
set -u
cd "$(dirname "$0")/.." || exit 1
[ $# -ge 2 ] || { echo "usage: $0 <jobs> <make-target>..." >&2; exit 2; }
jobs=$1
shift
mkdir -p out/test-logs
for t in "$@"; do rm -f "out/test-logs/$t.rc"; done

printf '%s\n' "$@" | MAKE_CMD=${MAKE:-make} xargs -P "$jobs" -I{} sh -c '
	t=$1
	log=out/test-logs/$t.log
	start=$(date +%s)
	if $MAKE_CMD --no-print-directory "$t" > "$log" 2>&1; then rc=0; else rc=1; fi
	if [ "$rc" = 0 ]; then word="ok   "; else word=FAIL; fi
	printf "%s %s (%s s)\n" "$word" "$t" "$(( $(date +%s) - start ))"
	echo "$rc" > "out/test-logs/$t.rc"
' _ {}

status=0
for t in "$@"; do
	if [ "$(cat "out/test-logs/$t.rc" 2>/dev/null)" != 0 ]; then
		status=1
		echo
		echo "== $t failed, its output (out/test-logs/$t.log):"
		cat "out/test-logs/$t.log" 2>/dev/null
	fi
done
exit $status
