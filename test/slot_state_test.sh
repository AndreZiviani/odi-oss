#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/slot-state.sh, off the device. nv is a stub
# that prints the two environment copies in the shape the real one does
# (nv getenv: "Valid environment: N" then the pairs; nv fallback:
# "Fallback environment: N" then the pairs, or an error and exit 1 when
# there is no valid second copy); /proc/cmdline and /proc/mtd are
# fixtures. Checks the state file, the motd and the syslog line for every
# shape the environment can take. `nv commit` itself is tested in
# src/nv/test.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/slot-state.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() {
	echo "FAIL  $1"
	for f in state motd log; do [ -f "$T/$f" ] && sed "s/^/        $f: /" "$T/$f"; done
	fail=$((fail + 1))
}
# st <what> <KEY=value>: that line is in the state file.
st()    { if grep -qx -- "$2" "$T/state"; then ok "$1"; else no "$1 (wanted '$2' in the state file)"; fi; }
motd()  { if grep -q -- "$2" "$T/motd"; then ok "$1"; else no "$1 (wanted /$2/ in the motd)"; fi; }
quiet() { if [ ! -s "$T/motd" ] && [ ! -s "$T/log" ]; then ok "$1"; else no "$1 (wanted an empty motd and no log line)"; fi; }
logged() { if grep -q -- "$2" "$T/log" 2>/dev/null; then ok "$1"; else no "$1 (wanted /$2/ in syslog)"; fi; }

mkdir -p "$T/bin"
cat > "$T/bin/nv" <<EOF
#!/bin/sh
p=\$(cat "$T/primary"); f=\$((3 - p))
case "\$1" in
getenv)
	[ -s "$T/env\$p" ] || { echo "nv: no valid environment in env or env2"; exit 1; }
	echo "Valid environment: \$p"; cat "$T/env\$p"; echo; exit 0 ;;
fallback)
	[ -s "$T/env\$f" ] || { echo "nv: no valid second copy of the environment"; exit 1; }
	echo "Fallback environment: \$f"; cat "$T/env\$f"; echo; exit 0 ;;
esac
exit 1
EOF
cat > "$T/bin/logger" <<EOF
#!/bin/sh
shift 4
echo "\$*" >> "$T/log"
EOF
# Only where the host has none (macOS): the bounds are a device concern.
if ! command -v timeout >/dev/null 2>&1; then
	printf '#!/bin/sh\nshift\nexec "$@"\n' > "$T/bin/timeout"
fi
chmod +x "$T/bin/"*
printf 'mtd5: 00300000 00001000 "r0"\nmtd7: 00300000 00001000 "r1"\n' > "$T/mtd"
: > "$T/devlog"

# env1 / env2: the pairs of each copy; $T/primary: which one U-Boot boots.
setup() {
	rm -f "$T/state" "$T/motd" "$T/log"
	printf 'bootdelay=1\nsw_active=%s\nsw_commit=%s\nsw_tryactive=%s\n' "$2" "$3" "$4" > "$T/env1"
	printf 'bootdelay=1\nsw_active=%s\nsw_commit=%s\nsw_tryactive=%s\n' "$5" "$6" "$7" > "$T/env2"
	echo "$1" > "$T/primary"
	echo "console=ttyS0,115200 root=31:7 rootfstype=squashfs" > "$T/cmdline"
}
run() {
	env PATH="$T/bin:$PATH" NV="$T/bin/nv" LOGGER="$T/bin/logger" \
	    PROC_CMDLINE="$T/cmdline" PROC_MTD="$T/mtd" STATE="$T/state" \
	    MOTD="$T/motd" DEV_LOG="$T/devlog" sh "$S" > "$T/out" 2>&1
	echo $? > "$T/rc"
}

echo "== a trial: slot 1 running, both copies still commit slot 0"
#     primary  env1: active commit try    env2: active commit try
setup 2        0 0 1                      1 0 2
run
st "running slot from root=31:7" "running=1"
st "sw_active from the primary copy" "sw_active=1"
st "the primary copy is named" "primary_copy=2"
st "primary sw_commit" "primary_sw_commit=0"
st "the fallback copy is named" "fallback_copy=1"
st "fallback sw_commit" "fallback_sw_commit=0"
st "the next reset boots slot 0" "next_boot=0"
st "uncommitted=1" "uncommitted=1"
motd "the motd says TRIAL BOOT" "TRIAL BOOT: running slot 1, which is not committed (sw_commit=0 in both copies)"
motd "and where the next reboot goes" "The next reboot returns to slot 0"
motd "and how to commit" "Commit with: nv commit 1 && slot-state.sh"
logged "and it goes to syslog" "TRIAL BOOT: running slot 1"
if [ "$(cat "$T/rc")" = 0 ]; then ok "exit 0"; else no "exit 0"; fi

echo "== committed in both copies: nothing to say"
setup 2        1 1 2                      1 1 2
run
st "uncommitted=0" "uncommitted=0"
st "next_boot is the running slot" "next_boot=1"
quiet "empty motd, no syslog line"

echo "== committed in the primary copy only (a plain nv setenv)"
setup 2        0 0 1                      1 1 2
run
st "uncommitted=1" "uncommitted=1"
st "next_boot is still this slot" "next_boot=1"
motd "HALF COMMITTED, naming the fallback copy" "HALF COMMITTED: running slot 1 is committed in the primary copy (2), but the fallback copy (1) still says sw_commit=0"
motd "and the fix" "Fix with: nv commit 1"

echo "== committed in the fallback copy only"
setup 2        1 1 2                      1 0 2
run
st "uncommitted=1" "uncommitted=1"
motd "says which copy disagrees" "committed only in the fallback copy (1); the primary copy (2) says sw_commit=0"
motd "and where the next reboot goes" "The next reboot returns to slot 0"

echo "== the two copies disagree with each other and with the running slot"
setup 1        1 0 2                      0 2 2
run
st "an invalid sw_commit reads as empty" "fallback_sw_commit="
motd "both values named" "sw_commit=0 in the primary copy (1), unset in the fallback copy (2)"

echo "== a new trial already armed: next_boot follows sw_tryactive"
setup 2        1 1 2                      1 1 0
run
st "committed" "uncommitted=0"
st "but the next reset boots the armed trial" "next_boot=0"

echo "== slot 0 running"
setup 1        0 1 2                      1 1 0
echo "console=ttyS0 root=31:5" > "$T/cmdline"
run
st "running=0" "running=0"
motd "TRIAL BOOT of slot 0" "TRIAL BOOT: running slot 0, which is not committed (sw_commit=1 in both copies). The next reboot returns to slot 1. Commit with: nv commit 0"

echo "== no valid fallback copy"
setup 2        0 0 1                      1 1 2
: > "$T/env1"
run
st "fallback_copy is empty" "fallback_copy="
st "committed, on the one copy there is" "uncommitted=0"
quiet "nothing to say"
setup 2        0 0 1                      1 0 2
: > "$T/env1"
run
motd "a trial with one copy says so" "sw_commit=0, and there is no valid fallback copy"

echo "== the state cannot be told"
setup 2        0 0 1                      1 0 2
echo "console=ttyS0 rdinit=/init" > "$T/cmdline"
run
st "no running slot" "running="
st "uncommitted is empty, not 0 or 1" "uncommitted="
motd "the motd says unknown" "SLOT STATE UNKNOWN: no root=31:N"
setup 2        0 0 1                      1 0 2
echo "console=ttyS0 root=31:9" > "$T/cmdline"
run
st "root= naming neither r0 nor r1" "running="
setup 2        0 0 1                      1 0 2
: > "$T/env1"; : > "$T/env2"
run
st "no environment: running slot still recorded" "running=1"
st "and uncommitted is empty" "uncommitted="
motd "the motd says so" "nv could not read a valid U-Boot environment"

echo "== files are replaced by rename and nothing else is left behind"
setup 2        0 0 1                      1 0 2
run
if [ -z "$(find "$T" -maxdepth 1 -name '*.tmp.*')" ]; then ok "no temp files left"; else no "no temp files left"; fi
if [ "$(wc -l < "$T/state" | tr -d ' ')" = 9 ]; then ok "the state file has its nine keys"; else no "the state file has its nine keys"; fi

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
