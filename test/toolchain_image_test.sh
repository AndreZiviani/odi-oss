#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# toolchain/image.sh against a stub docker: an image already here is not
# pulled, a pull that fails is tried again, and a pull that hangs is cut at
# PULL_TIMEOUT and fails the script with a message after PULL_TRIES attempts.
set -u
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
ok()  { printf 'ok    %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }

if ! command -v timeout >/dev/null 2>&1; then
	echo "toolchain_image_test: no timeout(1) on this host; skipped"
	exit 0
fi

# The stub: `image inspect` succeeds when $T/present exists; `pull` does what
# $T/mode says (ok, fail, hang, or failonce: fail the first call, then ok)
# and counts its calls in $T/pulls.
mkdir -p "$T/bin"
cat > "$T/bin/docker" <<EOF
#!/bin/sh
case "\$1 \$2" in
"image inspect") [ -e "$T/present" ] ;;
"pull -q")
	echo x >> "$T/pulls"
	case \$(cat "$T/mode") in
	ok) exit 0 ;;
	fail) exit 1 ;;
	hang) exec sleep 60 ;;
	failonce) [ "\$(wc -l < "$T/pulls")" -gt 1 ] ;;
	esac ;;
*) exit 2 ;;
esac
EOF
chmod +x "$T/bin/docker"

run() {   # run <mode>: image.sh diag against the stub; sets out, rc, pulls
	echo "$1" > "$T/mode"
	: > "$T/pulls"
	out=$(env PATH="$T/bin:$PATH" PULL_TIMEOUT=1 PULL_TRIES=2 PULL_PAUSE=0 \
		DIAG_IMAGE=ghcr.io/example/toolchain:v1 sh toolchain/image.sh diag 2>&1)
	rc=$?
	pulls=$(wc -l < "$T/pulls" | tr -d ' ')
}

touch "$T/present"
run ok
[ "$rc" = 0 ] && ok "an image already here: exit 0" || bad "an image already here: exit $rc"
[ "$pulls" = 0 ] && ok "and it is not pulled" || bad "an image already here was pulled $pulls times"
rm -f "$T/present"

run ok
[ "$rc" = 0 ] && [ "$pulls" = 1 ] && ok "a good pull: one attempt, exit 0" || bad "a good pull: exit $rc after $pulls"
case $out in *ghcr.io/example/toolchain:v1) ok "and it prints the reference" ;; *) bad "a good pull printed: $out" ;; esac

run failonce
[ "$rc" = 0 ] && [ "$pulls" = 2 ] && ok "a pull that fails once is tried again and passes" || bad "fail once: exit $rc after $pulls"
case $out in *"pull attempt 1 of 2 failed"*) ok "and it says so" ;; *) bad "fail once said: $out" ;; esac

run fail
[ "$rc" = 1 ] && [ "$pulls" = 2 ] && ok "a pull that keeps failing: PULL_TRIES attempts, exit 1" || bad "keeps failing: exit $rc after $pulls"
case $out in *"could not pull ghcr.io/example/toolchain:v1 (2 attempts)"*) ok "and it names the image" ;; *) bad "keeps failing said: $out" ;; esac

start=$(date +%s)
run hang
took=$(( $(date +%s) - start ))
[ "$rc" = 1 ] && [ "$pulls" = 2 ] && ok "a pull that hangs is cut and tried again, then exit 1" || bad "hang: exit $rc after $pulls"
[ "$took" -lt 10 ] && ok "within the bounds ($took s for two 1 s attempts)" || bad "a hanging pull took $took s"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
