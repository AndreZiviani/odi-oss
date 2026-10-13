#!/usr/bin/env bash
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
	if grep -q "^\[ -f /etc/config/$n\.off \] && exec /etc/scripts/respawn-off\.sh" "$f" 2>/dev/null; then
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
svc() { env PATH="$T/bin:$PATH" CONFIG_DIR="$T/config" WDT_REGISTER="$T/register" sh "$S" "$@" >/dev/null 2>&1; }
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

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
