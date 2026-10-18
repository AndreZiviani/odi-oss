#!/bin/sh
# ntp-event.sh -- busybox ntpd runs this (ntpd -S) on every event it has a
# name for: $1 is unsync (no peer answered for 8 polls), step, stratum or
# periodic, with stratum, offset (seconds), freq_drift_ppm and poll_interval
# in the environment. It leaves the answer to "is this clock synced?" in
# /tmp/ntp-status for the web UI, as plain key=value lines.
#
# ntpd starts this without waiting and does not serialise runs: a step and
# its stratum event can arrive milliseconds apart. So the file is written to
# a private temp name and renamed over the old one, which a reader sees
# whole or not at all, and nothing here may block or read anything slow.
#
# last_sync is the time of the last event that was not unsync. An unsync
# event carries it over from the file it replaces; empty means never synced.
# shellcheck disable=SC2154  # offset and stratum come from ntpd's environment
OUT=${NTP_STATUS_FILE:-/tmp/ntp-status}
TMP=$OUT.$$

now=$(date +%s)
last=
case "$1" in
unsync)
	last=$(sed -n 's/^last_sync=//p' "$OUT" 2>/dev/null)
	;;
*)
	last=$now
	;;
esac

{
	printf 'event=%s\n' "$1"
	printf 'time=%s\n' "$now"
	printf 'offset=%s\n' "$offset"
	printf 'stratum=%s\n' "$stratum"
	printf 'last_sync=%s\n' "$last"
} > "$TMP" && mv "$TMP" "$OUT"
rm -f "$TMP"
exit 0
