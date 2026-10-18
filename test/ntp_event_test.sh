#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/ntp-event.sh and the status-file handling in
# svc-ntpd.sh, off the device. The events are fed the way busybox ntpd feeds
# them (the action as $1, the numbers in the environment); the status file
# goes to a temporary directory through NTP_STATUS_FILE.
#
# What is checked: the five keys on every event, last_sync set by each event
# but unsync and carried over by unsync (empty when never synced), no temp
# file left behind, and that svc-ntpd.sh passes -S and clears the file on
# every path.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/ntp-event.sh
V=$PWD/rootfs/skeleton/etc/scripts/svc-ntpd.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
# eq NAME WANT GOT
eq() { if [ "$2" = "$3" ]; then ok "$1"; else no "$1" "wanted [$2], got [$3]"; fi; }

export NTP_STATUS_FILE=$T/ntp-status
# ev ACTION OFFSET STRATUM: one ntpd event, in a clean environment.
ev() { env -i PATH="$PATH" NTP_STATUS_FILE="$NTP_STATUS_FILE" offset="$2" stratum="$3" \
	freq_drift_ppm=0 poll_interval=64 sh "$S" "$1"; }
# key KEY: that line's value.
key() { sed -n "s/^$1=//p" "$NTP_STATUS_FILE"; }

ev unsync 0.000000 16
eq "unsync with no history: event"     unsync "$(key event)"
eq "unsync with no history: last_sync empty" "" "$(key last_sync)"
eq "unsync: stratum as given"          16 "$(key stratum)"

t0=$(date +%s)
ev step -0.250000 2
eq "step: event"        step "$(key event)"
eq "step: offset as given" -0.250000 "$(key offset)"
eq "step: stratum"      2 "$(key stratum)"
now=$(key time)
[ "$now" -ge "$t0" ] && [ "$now" -le $((t0 + 5)) ] && ok "time is now" || no "time is now" "$now vs $t0"
eq "step: last_sync is that event" "$now" "$(key last_sync)"
eq "exactly five keys" 5 "$(wc -l < "$NTP_STATUS_FILE" | tr -d ' ')"
eq "keys in order" "event time offset stratum last_sync" \
	"$(sed 's/=.*//' "$NTP_STATUS_FILE" | tr '\n' ' ' | sed 's/ $//')"

sync1=$(key last_sync)
sleep 1
ev unsync 0.000000 16
eq "unsync after sync: event"       unsync "$(key event)"
eq "unsync keeps the earlier last_sync" "$sync1" "$(key last_sync)"
[ "$(key time)" -gt "$sync1" ] && ok "unsync time moves on" || no "unsync time moves on"

ev periodic 0.001000 3
eq "periodic: event" periodic "$(key event)"
eq "periodic counts as synced" "$(key time)" "$(key last_sync)"

ev stratum 0.002000 4
eq "stratum: event" stratum "$(key event)"

ev periodic "" 3
eq "empty offset stays empty" "" "$(key offset)"
eq "no temp file left" "ntp-status" "$(ls "$T")"

# svc-ntpd.sh: the status file is removed before every exit path.
mkdir -p "$T/fs/tmp"
sed -e "s|/tmp/ntp-status|$T/fs/tmp/ntp-status|" "$V" > "$T/svc.sh"
grep -q -- '-S /etc/scripts/ntp-event.sh' "$V" && ok "svc-ntpd.sh passes -S" || no "svc-ntpd.sh passes -S"
printf 'event=step\n' > "$T/fs/tmp/ntp-status"
# With no ntpd binary at /bin/ntpd on a host (or no server), the script ends
# in respawn-off.sh; stub it so it returns.
sed -i.bak -e 's|exec /etc/scripts/respawn-off.sh|exit 0|' -e 's|/etc/scripts/flash|true|' "$T/svc.sh"
sh "$T/svc.sh"
[ ! -e "$T/fs/tmp/ntp-status" ] && ok "status removed when NTP is off or unset" || no "status removed when NTP is off or unset"

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
