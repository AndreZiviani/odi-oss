#!/usr/bin/env bash
#
# Install Debian packages on a CI runner, with every step bounded.
#
#   tools/ci-apt.sh <package>...
#
# The runner's apt mirror sometimes stalls a connection without closing it,
# and apt then waits on it with no limit: a step that normally takes 25 s
# took 11 minutes on one run of test-qemu and ran the whole job into its
# timeout on another. So each connection gets a read timeout and retries
# inside apt, each apt call a wall-clock limit, and the whole sequence three
# attempts. --no-install-recommends keeps the download to what the package
# needs to run (for qemu-system-mips, not the GUI, OpenGL or spice modules).
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <package>..." >&2; exit 1; }

opts=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)
for try in 1 2 3; do
	if timeout 180 sudo apt-get "${opts[@]}" update -qq &&
	   timeout 300 sudo apt-get "${opts[@]}" install -y -qq --no-install-recommends "$@"; then
		exit 0
	fi
	echo "ci-apt: attempt $try for $* failed or timed out" >&2
	[ "$try" = 3 ] || sleep 10
done
exit 1
