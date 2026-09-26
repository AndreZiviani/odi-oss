#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/fwu_starter.sh, off the device.
#
# Everything that decides whether a write may start is checked here: the
# inactive-slot rule (from /proc/cmdline, then sw_active, and a refusal when
# neither says), the fwu.sh md5 check against md5.txt, the one-writer lock,
# and that the job it starts records its outcome. fwu.sh is a stub that
# prints its arguments: the real one erases flash, and has its own test
# (fwu_guard_test.sh).
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/fwu_starter.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
t() {
	if printf '%s' "$3" | grep -q -- "$2"; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        wanted /$2/, got:"; printf '%s\n' "$3" | sed 's/^/        /'; fail=$((fail + 1)); fi
}

cat > "$T/mtd" <<'MTD'
dev:    size   erasesize  name
mtd4: 0014c000 00001000 "k0"
mtd5: 00274000 00001000 "r0"
mtd6: 0014c000 00001000 "k1"
mtd7: 00274000 00001000 "r1"
MTD
mkdir -p "$T/bin" "$T/img"
# setsid is not on every host this runs on; the stub runs the job in place,
# which is what setsid does apart from the new session.
printf '#!/bin/sh\nexec "$@"\n' > "$T/bin/setsid"
cat > "$T/bin/nv" <<'NV'
#!/bin/sh
[ -n "${NV_ACTIVE:-}" ] && echo "sw_active=$NV_ACTIVE"
exit 0
NV
chmod +x "$T/bin/setsid" "$T/bin/nv"
md5_of() { (md5sum "$1" 2>/dev/null || md5 -r "$1") | cut -d' ' -f1; }

mkimg() {   # mkimg <tar> [bad]
	printf '#!/bin/sh\necho "fwu stub slot=$1 tar=$2"\nexit ${FWU_STUB_RC:-0}\n' > "$T/img/fwu.sh"
	if [ "${2:-}" = bad ]; then
		echo "00000000000000000000000000000000  fwu.sh" > "$T/img/md5.txt"
	else
		echo "$(md5_of "$T/img/fwu.sh")  fwu.sh" > "$T/img/md5.txt"
	fi
	( cd "$T/img" && tar -cf "$1" fwu.sh md5.txt )
}
mkimg "$T/good.tar"
mkimg "$T/bad.tar" bad

run() {   # run <cmdline> [args...]
	echo "$1" > "$T/cmdline"; shift
	env PATH="$T/bin:$PATH" PROC_MTD="$T/mtd" PROC_CMDLINE="$T/cmdline" NV="$T/bin/nv" \
	    FWU_DIR="$T/fwu.d" FWU_LOG="$T/fwu.log" FWU_STATE="$T/fwu.state" \
	    sh "$S" "$@" 2>&1
}

t "a slot that is not 0 or 1 is refused" "usage" "$(run 'root=31:5' 2 "$T/good.tar")"
t "a missing tarball is refused" "no such image" "$(run 'root=31:5' 1 "$T/none.tar")"
t "the running slot is refused (cmdline names r0)" "running; write slot 1" "$(run 'root=31:5' 0 "$T/good.tar")"
t "the running slot is refused (cmdline names r1)" "running; write slot 0" "$(run 'root=31:7' 1 "$T/good.tar")"
t "sw_active answers when the cmdline does not" "running; write slot 0" \
  "$(NV_ACTIVE=1 run 'console=ttyS0' 1 "$T/good.tar")"
t "neither answering refuses both slots" "cannot tell which slot" "$(run 'console=ttyS0' 1 "$T/good.tar")"
t "a fwu.sh that does not match md5.txt is refused" "md5 mismatch" "$(run 'root=31:5' 1 "$T/bad.tar")"
t "a tar without fwu.sh is refused" "no fwu.sh" "$( : > "$T/empty"; ( cd "$T" && tar -cf notimg.tar empty ); run 'root=31:5' 1 "$T/notimg.tar")"

out=$(run 'root=31:5' --foreground 1 "$T/good.tar")
t "foreground runs fwu.sh with the slot and the tarball" "fwu stub slot=1 tar=$T/good.tar" "$out"

rm -f "$T/fwu.state"
out=$(run 'root=31:5' 1 "$T/good.tar")
t "background returns at once and says where the log is" "in the background" "$out"
for _ in 1 2 3 4 5; do grep -q '^ok' "$T/fwu.state" 2>/dev/null && break; sleep 1; done
t "the job records its outcome" "^ok [0-9]* 1" "$(cat "$T/fwu.state")"
t "and its output lands in the log" "fwu stub slot=1" "$(cat "$T/fwu.log")"

rm -f "$T/fwu.state"
FWU_STUB_RC=3 run 'root=31:5' 1 "$T/good.tar" > /dev/null
for _ in 1 2 3 4 5; do grep -q '^failed' "$T/fwu.state" 2>/dev/null && break; sleep 1; done
t "a failing fwu.sh is recorded with its exit code" "^failed [0-9]* 1 3" "$(cat "$T/fwu.state")"

# A live writer holds the lock; a dead one does not.
sleep 30 & live=$!
echo "running $live 1" > "$T/fwu.state"
t "a second write while one runs is refused" "already running" "$(run 'root=31:5' 1 "$T/good.tar")"
kill "$live" 2>/dev/null; wait "$live" 2>/dev/null
t "a stale running state does not block" "in the background" "$(run 'root=31:5' 1 "$T/good.tar")"
sleep 1

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
