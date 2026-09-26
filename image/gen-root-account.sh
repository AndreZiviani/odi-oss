#!/usr/bin/env bash
#
# Writes the root account files for the image: /etc/passwd's one line for
# root, the /etc/odi-keys-only marker, and out/image/root-password*.txt.
# Split out of build.sh so the three shapes are testable without the rest
# of the build (no kernel, no busybox, no docker) -- see test/root_pw_test.sh.
# build.sh does the password generation and the SHA-512 crypt hashing (the
# hashing runs in the toolchain container, via build.sh's `run`); this
# script only writes files, given the finished values.
#
# Usage: gen-root-account.sh <STAGE> <OUT> <VERSION> <MODE> [PW] [HASH]
#   MODE=locked     no password at all. Ships /etc/odi-keys-only, which
#                    /etc/init.d/services reads to start dropbear with -s
#                    (refuse password logins outright). First access is the
#                    web UI (confd), built-in admin/admin until a password
#                    is set, which can add an SSH key for root. See
#                    docs/BUILDING.md and docs/FLASHING.md.
#   MODE=none        root has an empty password field (ROOT_PW=none).
#   MODE=password    PW (plaintext) and HASH ($6$ SHA-512 crypt) are both
#                    given; written to /etc/passwd and
#                    out/image/root-password*.txt. This is the default.
set -euo pipefail
STAGE=$1 OUT=$2 VERSION=$3 MODE=$4 PW=${5:-} HASH=${6:-}
mkdir -p "$OUT"
# Never carry the marker over from a previous call at the same STAGE: only
# MODE=locked below re-creates it.
rm -f "$STAGE/etc/odi-keys-only"

case "$MODE" in
locked)
	# "!": a hash no crypt() ever produces, so a password check always
	# fails, even before dropbear's own -s (services) refuses password
	# auth at the protocol level.
	echo 'root:!:0:0:root:/root:/bin/sh' > "$STAGE/etc/passwd"
	: > "$STAGE/etc/odi-keys-only"
	rm -f "$OUT/root-password.txt"
	;;
none)
	echo 'root::0:0:root:/root:/bin/sh' > "$STAGE/etc/passwd"
	rm -f "$OUT/root-password.txt"
	;;
password)
	[ -n "$PW" ] && [ -n "$HASH" ] ||
		{ echo "gen-root-account.sh: MODE=password needs PW and HASH" >&2; exit 1; }
	echo "root:$HASH:0:0:root:/root:/bin/sh" > "$STAGE/etc/passwd"
	printf '%s\n' "$PW" > "$OUT/root-password.txt"
	chmod 600 "$OUT/root-password.txt"
	# And a copy named for this version: the file above is overwritten by
	# the next build, which locks you out of an image still running on a
	# stick. Never cleaned by the build.
	printf '%s\n' "$PW" > "$OUT/root-password-$VERSION.txt"
	chmod 600 "$OUT/root-password-$VERSION.txt"
	;;
*)
	echo "gen-root-account.sh: unknown MODE $MODE (want locked, none or password)" >&2
	exit 1
	;;
esac
chmod 644 "$STAGE/etc/passwd"
