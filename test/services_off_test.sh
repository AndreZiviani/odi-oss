#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# /etc/init.d/services `stop <name>` writes /etc/config/<name>.off and kills
# the daemon; it stays down only if that daemon's respawn entry,
# svc-<name>.sh, checks the same flag before it execs. svc-omcid.sh checked
# only modules.off, so `services stop omcid` was undone by the respawn a
# moment later. Every name `services` accepts must have the matching check.
set -u
cd "$(dirname "$0")/.." || exit 1
S=rootfs/skeleton/etc/init.d/services
D=rootfs/skeleton/etc/scripts
pass=0; fail=0
names=$(sed -n 's/.*{stop|start} {\([a-z|]*\)}.*/\1/p' "$S" | tr '|' ' ')
if [ -z "$names" ]; then echo "FAIL  could not read the service list from $S"; exit 1; fi
for n in $names; do
	f=$D/svc-$n.sh
	if grep -Eq "^\[ -f /etc/config/$n\.off \] && (\[ ! -f /var/run/safe-mode \] && )?exec /etc/scripts/respawn-off\.sh" "$f" 2>/dev/null; then
		echo "ok    svc-$n.sh honours $n.off"; pass=$((pass + 1))
	else
		echo "FAIL  svc-$n.sh does not check /etc/config/$n.off"; fail=$((fail + 1))
	fi
done

# omcid is a watchdog client: stop releases its deadline (register with 0)
# before the kill, start registers it again. Run against a scratch config
# dir and a plain file standing in for /proc/odi_wdt/register.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/config"
printf '#!/bin/sh\nshift\nexec "$@"\n' > "$T/bin/timeout"; chmod +x "$T/bin/timeout"
: > "$T/register"
svc() { env PATH="$T/bin:$PATH" CONFIG_DIR="$T/config" WDT_REGISTER="$T/register" SAFE_FLAG="$T/no-safe-mode" sh "$S" "$@" >/dev/null 2>&1; }
check() {
	if [ "$2" = "$3" ]; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1: wanted '$2', got '$3'"; fail=$((fail + 1)); fi
}
svc stop omcid
check "stop omcid writes omcid.off" yes "$([ -f "$T/config/omcid.off" ] && echo yes)"
check "stop omcid releases its watchdog deadline" "omcid 0" "$(cat "$T/register")"
svc start omcid
check "start omcid removes omcid.off" no "$([ -f "$T/config/omcid.off" ] && echo yes || echo no)"
check "start omcid registers the deadline again" "omcid 60" "$(cat "$T/register")"
: > "$T/register"
svc stop confd
check "stop confd leaves the watchdog alone" "" "$(cat "$T/register")"

# A safe-mode boot (safe-mode.sh) is the way back into a stick that cannot be
# reached: confd and dropbear start in it whatever their .off file says, and
# the file stays for the next normal boot. Each entry is run from a copy with
# its absolute paths pointed into $T; respawn.sh and respawn-off.sh are stubs
# that say which one was reached.
mkdir -p "$T/scripts" "$T/run" "$T/sbin"
printf '#!/bin/sh\necho "respawn $*"\n' > "$T/scripts/respawn.sh"
printf '#!/bin/sh\necho off\n' > "$T/scripts/respawn-off.sh"
: > "$T/confd"; : > "$T/sbin/dropbear"
# dropbearkey: writes a key unless the path is under a directory named ro
printf '#!/bin/sh\ncase "$4" in */ro/*) exit 1 ;; esac\necho key > "$4"\n' > "$T/sbin/dropbearkey"
chmod +x "$T/scripts/respawn.sh" "$T/scripts/respawn-off.sh" "$T/confd" "$T/sbin/dropbear" "$T/sbin/dropbearkey"
entry() {   # entry <name> <config dir>
	sed -e "s#/etc/scripts/#$T/scripts/#g" -e "s#/etc/config#$2#g" -e "s#/var/run/#$T/run/#g" \
	    -e "s#/bin/confd#$T/confd#g" -e "s#/sbin/dropbear#$T/sbin/dropbear#g" \
	    -e "s#/etc/odi-keys-only#$T/keys-only#g" "$D/svc-$1.sh" > "$T/svc-$1.sh"
	sh "$T/svc-$1.sh" 2>/dev/null
}
for n in confd dropbear; do
	rm -f "$T/run/safe-mode"; : > "$T/config/$n.off"
	check "$n.off parks $n in a normal boot" off "$(entry $n "$T/config")"
	: > "$T/run/safe-mode"
	check "a safe-mode boot starts $n whatever $n.off says" "respawn $n" "$(entry $n "$T/config" | cut -d' ' -f1-2)"
	check "and leaves $n.off where it is" yes "$([ -f "$T/config/$n.off" ] && echo yes)"
	rm -f "$T/config/$n.off"
done
: > "$T/config/omcid.off"
check "a safe-mode boot still honours omcid.off" 1 "$(grep -c '^\[ -f /etc/config/omcid\.off \] && exec' "$D/svc-omcid.sh")"
rm -f "$T/config/omcid.off"

# No host key and nowhere to keep one: a normal boot parks dropbear, a
# safe-mode boot makes a key in RAM for that boot.
mkdir -p "$T/ro"
rm -f "$T/run/safe-mode" "$T/run/dropbear-safe-mode.ed25519"
check "no host key and none can be written: dropbear parked in a normal boot" off "$(entry dropbear "$T/ro")"
: > "$T/run/safe-mode"
out=$(entry dropbear "$T/ro")
check "a safe-mode boot runs dropbear on a key in RAM" "-r $T/run/dropbear-safe-mode.ed25519" "$(echo "$out" | grep -o -- "-r [^ ]*")"
check "and the key is there" yes "$([ -s "$T/run/dropbear-safe-mode.ed25519" ] && echo yes)"
out=$(entry dropbear "$T/config")
check "a safe-mode boot with a writable config keeps the stored key" "-r $T/config/dropbear.d/ed25519" "$(echo "$out" | grep -o -- "-r [^ ]*")"
rm -f "$T/run/safe-mode"

# A safe-mode boot must stay up whatever state omcid is in: rcS does not
# register the watchdog client there, and `services start omcid` does not
# arm it either.
R=rootfs/skeleton/etc/init.d/rcS
if awk '/^if \[ -f \/var\/run\/safe-mode \]; then$/ { s = NR } /^elif .*\/proc\/odi_wdt\/register/ { if (s && NR == s + 2) ok = 1 } END { exit !ok }' "$R"; then
	echo "ok    rcS skips the omcid watchdog registration in a safe-mode boot"; pass=$((pass + 1))
else echo "FAIL  rcS registers omcid with odi_wdt in a safe-mode boot"; fail=$((fail + 1)); fi
check "rcS registers omcid nowhere else" 1 "$(grep -c 'write_proc_bounded /proc/odi_wdt/register' "$R")"
: > "$T/register"; : > "$T/safe"
env PATH="$T/bin:$PATH" CONFIG_DIR="$T/config" WDT_REGISTER="$T/register" SAFE_FLAG="$T/safe" sh "$S" start omcid >/dev/null 2>&1
check "start omcid in a safe-mode boot does not arm the deadline" "" "$(cat "$T/register")"
env PATH="$T/bin:$PATH" CONFIG_DIR="$T/config" WDT_REGISTER="$T/register" SAFE_FLAG="$T/safe" sh "$S" stop omcid >/dev/null 2>&1
check "stop omcid in a safe-mode boot still releases it" "omcid 0" "$(cat "$T/register")"
rm -f "$T/config/omcid.off"

# a daemon in its restart wait is the respawn.sh shell, not yet the daemon:
# stop has to reach it too, or it execs the daemon after the flag is written
if grep -q 'kill_by_cmdline "respawn.sh \$svc "' "$S"; then echo "ok    stop also kills the respawn.sh restart wait"; pass=$((pass + 1))
else echo "FAIL  stop does not reach a daemon in its restart wait"; fail=$((fail + 1)); fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
