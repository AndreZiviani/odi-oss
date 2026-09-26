#!/usr/bin/env bash
# image/gen-root-account.sh: the three shapes of the root account file
# image/build.sh can write (ROOT_PW=locked, ROOT_PW=none, and the default
# per-build password), against a scratch STAGE/OUT -- no docker, no kernel,
# no busybox needed.
set -u
cd "$(dirname "$0")/.." || exit 1
G=image/gen-root-account.sh
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
t() {
	if [ "$2" = "$3" ]; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        want [$2]"; echo "        got  [$3]"; fail=$((fail + 1)); fi
}

fresh() {
	rm -rf "$T/stage" "$T/out"
	mkdir -p "$T/stage/etc" "$T/out"
}

# ROOT_PW=locked: no password can ever match, keys-only marker present, no
# password file left behind.
fresh
bash "$G" "$T/stage" "$T/out" v-locked locked
t "locked: passwd field is !"    "root:!:0:0:root:/root:/bin/sh" "$(cat "$T/stage/etc/passwd")"
t "locked: keys-only marker"     "yes"  "$([ -f "$T/stage/etc/odi-keys-only" ] && echo yes || echo no)"
t "locked: no password file"     "no"   "$([ -f "$T/out/root-password.txt" ] && echo yes || echo no)"
t "locked: passwd mode 644"      "644"  "$(stat -f%Lp "$T/stage/etc/passwd" 2>/dev/null || stat -c%a "$T/stage/etc/passwd")"

# ROOT_PW=none: empty password field, no keys-only marker (password auth is
# still offered, it just always succeeds with an empty password -- a
# development choice, not the release one).
fresh
bash "$G" "$T/stage" "$T/out" v-none none
t "none: passwd field is empty"  "root::0:0:root:/root:/bin/sh" "$(cat "$T/stage/etc/passwd")"
t "none: no keys-only marker"    "no"   "$([ -f "$T/stage/etc/odi-keys-only" ] && echo yes || echo no)"

# The default: a plaintext password and its hash, both supplied by the
# caller (build.sh computes them; this script only writes files).
fresh
bash "$G" "$T/stage" "$T/out" v260926-abc password s3cr3tpw '$6$rounds=5000$abc$hash'
t "password: passwd carries the hash" \
	'root:$6$rounds=5000$abc$hash:0:0:root:/root:/bin/sh' "$(cat "$T/stage/etc/passwd")"
t "password: root-password.txt"      "s3cr3tpw" "$(cat "$T/out/root-password.txt")"
t "password: versioned copy"         "s3cr3tpw" "$(cat "$T/out/root-password-v260926-abc.txt")"
t "password: no keys-only marker"    "no"   "$([ -f "$T/stage/etc/odi-keys-only" ] && echo yes || echo no)"
t "password: password file mode 600" "600"  "$(stat -f%Lp "$T/out/root-password.txt" 2>/dev/null || stat -c%a "$T/out/root-password.txt")"

# A stale marker from a previous, differently-configured call at the same
# STAGE must not survive into a mode that should not carry it.
fresh
bash "$G" "$T/stage" "$T/out" v-a locked
bash "$G" "$T/stage" "$T/out" v-b none
t "marker does not leak across calls" "no" "$([ -f "$T/stage/etc/odi-keys-only" ] && echo yes || echo no)"

# password mode refuses to run with no PW/HASH given.
fresh
if bash "$G" "$T/stage" "$T/out" v-x password 2>/dev/null; then
	echo "FAIL  password mode without PW/HASH should have failed"; fail=$((fail + 1))
else
	echo "ok    password mode without PW/HASH fails"; pass=$((pass + 1))
fi

echo "-- $pass ok, $fail failed --"
[ "$fail" -eq 0 ]
