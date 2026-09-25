#!/usr/bin/env bash
#
# image/fwu.sh must run on the STOCK vendor image, because that is what it is
# flashed FROM the first time. The stock userland is busybox 1.12.4 with 52
# applets, and it has no head, no wc, no dd, no tail, no tr and no `command -v`.
#
# This is the check that was missing. fwu.sh piped through `head -1` on the
# first thing it did, so on a real stick it printed
#
#     fwu: no MTD partition named k1
#
# and exited -- accusing the flash map, which is the one failure mode
# docs/TRIAL-BOOT.md teaches you to fear. Nothing was written, so it was
# fail-safe, but it cost the first flash. Neither of the other two fwu
# harnesses could see it: test/fwu_guard_test.sh runs the script under the
# HOST's bash and test/flash_harness_test.sh runs it under OUR busybox inside
# QEMU. Both supply an interpreter whose completeness is the thing in question.
#
# It is an ALLOWLIST on purpose. A list of commands known to be absent cannot
# find the next one -- that lesson was
# learned the expensive way, with an audit that looked only for the
# instructions already suspected and passed a binary full of `teq`.
set -u
cd "$(dirname "$0")/.." || exit 1

LIST=image/stock-commands.txt
FWU=${FWU:-image/fwu.sh}
pass=0
fail=0

ok()  { printf 'ok    %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }

[ -r "$LIST" ] || { echo "no $LIST"; exit 1; }
[ -r "$FWU" ]  || { echo "no $FWU"; exit 1; }

# The allowed set. An empty one must fail rather than let everything through:
# a check that finds nothing has to go red, or it passes hardest exactly when
# it is most broken.
allowed=$(grep -v '^#' "$LIST" | grep -v '^[[:space:]]*$' | sort -u)
n_allowed=$(printf '%s\n' "$allowed" | grep -c .)
if [ "$n_allowed" -lt 50 ]; then
	bad "the allowlist has $n_allowed entries -- $LIST has rotted"
	printf '\n%d passed, %d failed\n' "$pass" "$fail"
	exit 1
fi
ok "the allowlist carries $n_allowed commands"

# Words in command position: the start of a line, or after a pipe, a
# semicolon, an ampersand, a paren, or a brace. This over-approximates -- it
# also picks up the word after `then` or `do` on the same line, which is a
# command anyway -- and over-approximating is the safe direction: an extra
# word gets checked, a missed one does not.
#
# Comments and the CONTENTS of quoted strings go first. Without that, an
# ordinary message like "(run from the unpacked image)" is split on its parens
# and contributes `run` and `image` as command words.
code=$(sed -e "s/'[^']*'/''/g" -e 's/"[^"]*"/""/g' -e 's/#.*//' "$FWU")

used=$(
	printf '%s\n' "$code" |
	tr '|;&(){}`' '\n' |
	sed -e 's/^[[:space:]]*//' -e 's/^![[:space:]]*//' |
	awk '{ print $1 }' |
	grep -E '^[a-zA-Z_][a-zA-Z0-9_-]*$' |
	sort -u
)
n_used=$(printf '%s\n' "$used" | grep -c .)
if [ "$n_used" -lt 10 ]; then
	bad "only $n_used command words found in $FWU -- the tokeniser has rotted"
	printf '\n%d passed, %d failed\n' "$pass" "$fail"
	exit 1
fi
ok "found $n_used distinct command words in $FWU"

# Words this script defines for itself are not external commands.
selfdef=$(grep -E '^[a-zA-Z_][a-zA-Z0-9_]*\(\)' "$FWU" | sed 's/().*//' | sort -u)

unknown=""
for w in $used; do
	printf '%s\n' "$allowed"  | grep -qx "$w" && continue
	printf '%s\n' "$selfdef"  | grep -qx "$w" && continue
	unknown="$unknown $w"
done

if [ -n "$unknown" ]; then
	bad "not on the stock image:$unknown"
	echo "      If one of these really is there, add it to $LIST with the"
	echo "      evidence. Do not delete this test."
else
	ok "every command fwu.sh runs exists on the stock image"
fi

# The specific regressions, named so the failure says which one came back.
for c in head wc dd tail tr sort which basename dirname; do
	if printf '%s\n' "$used" | grep -qx "$c"; then
		bad "fwu.sh calls '$c', which the stock image does not have"
	fi
done
# Against the stripped code, not the file: this test's own prose says
# `command -v` several times and must not trip its own check.
printf '%s\n' "$code" | grep -q 'command[[:space:]]*-v' &&
	bad "fwu.sh uses 'command -v', which the stock ash does not implement"
ok "none of the known-absent commands is back"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
