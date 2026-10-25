#!/bin/sh
#
# mcast-probe.sh -- measure, on a stick, what the switch does with multicast
# and IGMP, with the ACL probes of `diag acl-probe` (docs/SWITCH.md,
# "Multicast", "What is not known" lists the questions and where each answer
# goes). Runs ON THE STICK, under busybox ash; it needs an image whose
# kernel and diag have the probes.
#
#     nohup sh /var/tmp/mcast-probe.sh > /var/tmp/mcast-probe.log 2>&1 &
#
# One run is about 15 minutes (QUICK=1: about 3, for a dry run of the
# script itself, too short to catch an OLT query interval). It ends with a
# results block, also in $OUT/results.txt, to paste into docs/SWITCH.md.
#
# What it changes, and puts back on every way out (the EXIT trap, and the
# kernel on top of it: the probes belong to one diag process fed from a
# FIFO here, and that process exiting, for whatever reason, removes them):
#
#   - ACL entries 88 to 95, template 4 and the log counter modes, through
#     diag acl-probe: counting rules on the UNI (port 0) and PON (port 2)
#     ingress, which change the forwarding of no frame, and for one window each a
#     copy (mirror) and a take (trap) of the IGMP entering the UNI;
#   - with ROW=1 (the default), for one window, a static L2 row for $GROUP
#     whose only member is port 1 (not wired), written and removed with
#     igmpd -w -j / -l. A run killed with SIGKILL during that window leaves
#     the row; `igmpd -w -l 239.255.77.1` removes it, a reboot too.
#
# What it never does: write flash or the config store, touch the U-Boot
# slot variables, start or stop a service, send anything itself, or read a
# register directly (every read goes through diag and /proc). It refuses to
# run while igmpd runs (the snooping lease and the probes exclude each
# other).
#
# The UNI half of every question needs IGMP and multicast from the host. The
# stick cannot make the host send them, so run this beside the script, on
# the host, on the interface that faces the stick (a Linux host; IFACE and
# the group as here; it joins and leaves the group every 15 s and sends
# twenty 1000-byte pings to it each time, with TTL 1):
#
#     end=$(( $(date +%s) + 1020 )); while [ "$(date +%s)" -lt "$end" ]; do
#       ip addr add 239.255.77.1/32 dev IFACE autojoin
#       ping -q -c 20 -i 0.2 -s 1000 -t 1 -I IFACE 239.255.77.1
#       ip addr del 239.255.77.1/32 dev IFACE; sleep 5; done
#
# Without it the PON half is still measured, and the UNI lines say they saw
# nothing.
#
# Settings, from the environment:
#   OUT        where the snapshots and results go (default /var/tmp/mcast-probe)
#   GROUP      the test group (default 239.255.77.1)
#   ROW        1: run the row window (default), 0: skip it
#   QUICK      1: every window 30 s
#   COUNT_S BYTES_S OVERLAP_S CPU_S ROW_S   window lengths, seconds
#   LABEL      a word for the results header (the line, say ISP1)
#   DEV PROC   /dev/odi_sw and /proc, for test/mcast_probe_test.sh only
#
# POSIX sh; busybox timeout, mkfifo, pidof, awk.

PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH
set -u
# Run under nohup; ignored as well, so a lost session never cuts a window.
trap '' HUP

OUT=${OUT:-/var/tmp/mcast-probe}
DEV=${DEV:-/dev/odi_sw}
PROC=${PROC:-/proc}
GROUP=${GROUP:-239.255.77.1}
ROW=${ROW:-1}
LABEL=${LABEL:-?}
if [ "${QUICK:-0}" = 1 ]; then
	COUNT_S=${COUNT_S:-30} BYTES_S=${BYTES_S:-30} OVERLAP_S=${OVERLAP_S:-30}
	CPU_S=${CPU_S:-30} ROW_S=${ROW_S:-30}
else
	COUNT_S=${COUNT_S:-300} BYTES_S=${BYTES_S:-120} OVERLAP_S=${OVERLAP_S:-60}
	CPU_S=${CPU_S:-120} ROW_S=${ROW_S:-120}
fi

# The bounds: one diag call, one igmpd one-shot, one poll for the holder.
DIAG_S=10
IGMPD_S=10
POLL_TENTHS=50

FIFO=$OUT/ctl
RES=$OUT/results.txt
HOLDER=
ROW_SET=
GROUP_MAC=
OVERALL_S=$((COUNT_S + BYTES_S + OVERLAP_S + 3 * CPU_S + ROW_S + 300))

# The log, kept on fd 5: a trap can run while the output of a command is
# redirected to a file, and its words belong in the log, not in that file.
exec 5>&1
say() { echo "mcast-probe: $*" >&5; }
die() { say "$*"; exit 1; }

# A child must not keep the FIFO open (fd 4): the holder stops at the end
# of input, and only the last writer closing ends it.
diag1() { timeout "$DIAG_S" diag "$@" 4>&-; }
# In the background and waited for: a shell runs a trap only once its
# foreground command is done, and wait is the one that a signal ends.
nap() {
	sleep "$1" 4>&- &
	wait $!
}

# ---- preconditions --------------------------------------------------------

mkdir -p "$OUT" || die "cannot create $OUT"
rm -f "$OUT"/*.mib "$OUT"/*.nic "$OUT"/*.omci "$OUT"/*.probe "$RES" "$FIFO"
[ -c "$DEV" ] || die "no $DEV: not an odi-oss image"
command -v diag >/dev/null || die "no diag"
if pidof igmpd >/dev/null 2>&1; then
	die "igmpd is running: the snooping lease and the probes exclude each other; run this with IGMP_SNOOPING unset"
fi
diag1 acl-probe show 0 > "$OUT/check.txt" 2>&1
grep -q '^slot=0 ' "$OUT/check.txt" ||
	die "this image has no ACL probes (diag acl-probe show 0: $(cat "$OUT/check.txt"))"
grep -q ' empty$' "$OUT/check.txt" || die "a probe is already installed by another process"

case $GROUP in
*.*.*.*) ;;
*) die "GROUP must be a.b.c.d" ;;
esac
GROUP_HEX=$(echo "$GROUP" | awk -F. '{ printf "0x%02x%02x%02x%02x", $1, $2, $3, $4 }')
GROUP_MAC=$(echo "$GROUP" | awk -F. '{ printf "01:00:5e:%02x:%02x:%02x", $2 % 128, $3, $4 }')

# ---- the holder -------------------------------------------------------------

# One diag, its commands from the FIFO: the probes are its, so they go when
# it does. timeout -s KILL bounds it whatever happens to this script.
start_holder() {
	mkfifo "$FIFO" || die "mkfifo $FIFO failed"
	timeout -s KILL "$OVERALL_S" diag < "$FIFO" > "$OUT/holder.out" 2>&1 &
	HOLDER=$!
	# Read and write: opening a FIFO this way does not wait for the other end.
	exec 4<>"$FIFO"
}

hold() { printf '%s\n' "$*" >&4; }

# Wait until slot $1 reads as installed (or, with $2 = empty, as empty).
wait_slot() {
	t=0
	while [ "$t" -lt "$POLL_TENTHS" ]; do
		diag1 acl-probe show "$1" > "$OUT/slot.txt" 2>&1
		if [ "${2:-}" = empty ]; then
			grep -q ' empty$' "$OUT/slot.txt" && return 0
		else
			grep -q ' port=' "$OUT/slot.txt" && return 0
		fi
		nap 0.1
		t=$((t + 1))
	done
	return 1
}

# probe SLOT ARGS...: install through the holder and wait for it.
probe() {
	hold "acl-probe $*"
	slot=$(echo "$*" | awk '{ print $2 }')
	wait_slot "$slot" || {
		say "probe '$*' did not go in; the holder said: $(sed -n '$p' "$OUT/holder.out")"
		exit 1
	}
}

clear_probes() {
	hold "acl-probe delete all"
	wait_slot 0 empty || die "the probes did not go"
}

# ---- snapshots ---------------------------------------------------------------

snap() {
	diag1 mib dump counter port 0,2,3 > "$OUT/$1.mib" 2>&1
	cat "$PROC/odi_nic" > "$OUT/$1.nic" 2>/dev/null
	cat "$PROC/odi_omci" > "$OUT/$1.omci" 2>/dev/null
	printf 'acl-probe show %s\n' 0 1 2 3 4 5 6 7 | timeout "$DIAG_S" diag 4>&- |
		grep '^slot=' > "$OUT/$1.probe"
	awk '{ print int($1) }' "$PROC/uptime" > "$OUT/$1.t"
}

# mib LABEL PORT NAME
mib() {
	awk -v p="$2" -v n="$3" '/^Port:/ { cur = $2 } cur == p && $1 == n { print $3; exit }' \
		"$OUT/$1.mib"
}

# mibd FROM TO PORT NAME: the difference, or ? when either is missing.
mibd() {
	a=$(mib "$1" "$3" "$4")
	b=$(mib "$2" "$3" "$4")
	if [ -n "$a" ] && [ -n "$b" ]; then echo $((b - a)); else echo '?'; fi
}

# procd FROM TO FILE KEY: a /proc counter difference.
procd() {
	a=$(awk -v k="$4" '$1 == k { print $2; exit }' "$OUT/$1.$3")
	b=$(awk -v k="$4" '$1 == k { print $2; exit }' "$OUT/$2.$3")
	if [ -n "$a" ] && [ -n "$b" ]; then echo $((b - a)); else echo '?'; fi
}

# pv LABEL SLOT KEY: one key of a probe line.
pv() {
	awk -v s="slot=$2" -v k="$3" '$1 == s {
		for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == k) { print kv[2]; exit } }
	}' "$OUT/$1.probe"
}

secs() { echo $(($(cat "$OUT/$2.t") - $(cat "$OUT/$1.t"))); }

# rate N SECONDS: per second, two decimals.
rate() {
	case $1 in
	*[!0-9]*|'') echo '?' ;;
	*) awk -v n="$1" -v s="$2" 'BEGIN { if (s > 0) printf "%.2f", n / s; else print "?" }' ;;
	esac
}

# per N D: N / D, one decimal, for bytes per frame.
per() {
	awk -v n="$1" -v d="$2" 'BEGIN { if (d + 0 > 0 && n ~ /^[0-9.]+$/) printf "%.1f", n / d; else print "?" }'
}

# window NAME SECONDS: the probes are in; snapshot, wait, snapshot.
window() {
	say "window $1: $2 s"
	snap "$1.a"
	nap "$2"
	snap "$1.b"
	# The same counters again a moment later, with nothing installed or
	# removed between: a counter that cleared on read would read low.
	nap 1
	snap "$1.c"
}

# ---- the way out --------------------------------------------------------------

cleanup() {
	trap '' INT TERM
	if [ -n "$ROW_SET" ]; then
		timeout "$IGMPD_S" igmpd -w -l "$GROUP" 4>&- > /dev/null 2>&1
		ROW_SET=
	fi
	if [ -n "$HOLDER" ]; then
		hold "acl-probe delete all" 2>/dev/null
		hold "exit" 2>/dev/null
		exec 4>&-
		t=0
		while kill -0 "$HOLDER" 2>/dev/null && [ "$t" -lt "$POLL_TENTHS" ]; do
			sleep 0.1
			t=$((t + 1))
		done
		kill -9 "$HOLDER" 2>/dev/null
		HOLDER=
	fi
	rm -f "$FIFO"
	printf 'acl-probe show %s\n' 0 1 2 3 4 5 6 7 | timeout "$DIAG_S" diag |
		grep '^slot=' > "$OUT/left.probe"
	left=$(grep -vc ' empty$' "$OUT/left.probe")
	# Eight lines, or the count above means nothing.
	[ "$(grep -c '^slot=' "$OUT/left.probe")" = 8 ] || left="? (the slots could not be read)"
	row='?'
	[ -n "$GROUP_MAC" ] && row=$(timeout "$DIAG_S" diag l2-table get all | grep -ic "^$GROUP_MAC")
	say "restored: probes left ${left:-?}, rows for $GROUP left ${row:-?}"
	[ -f "$RES" ] && echo "restored: probes left ${left:-?}, rows for $GROUP left ${row:-?}" >> "$RES"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---- the windows ---------------------------------------------------------------

start_holder
snap start

# Counting, frames. A frame is counted by the lowest slot that matches it,
# so the narrow rules sit below the wide ones: on each port IGMP first, then
# (UNI) the test group, then the rest of 224.0.0.0/4.
probe count 0 port 2 proto 2
probe count 1 port 2 group 0xe0000000 4
probe count 2 port 0 proto 2
probe count 3 port 0 group "$GROUP_HEX" 32
probe count 4 port 0 group 0xe0000000 4
window count "$COUNT_S"
clear_probes

# The same, in bytes.
probe bytes 0 port 2 proto 2
probe bytes 1 port 2 group 0xe0000000 4
probe bytes 2 port 0 proto 2
probe bytes 3 port 0 group "$GROUP_HEX" 32
probe bytes 4 port 0 group 0xe0000000 4
window bytes "$BYTES_S"
clear_probes

# Two rules for the same frames: which one counts.
probe count 0 port 2 group 0xe0000000 4
probe count 1 port 2 group 0xe0000000 4
window overlap "$OVERLAP_S"
clear_probes

# The IGMP of the host copied to the CPU, with the rest of the UNI multicast
# counted beside it.
probe mirror 0 igmp
probe count 3 port 0 group "$GROUP_HEX" 32
probe count 4 port 0 group 0xe0000000 4
window mirror "$CPU_S"
clear_probes

# Taken instead, the switch counting bytes: its count against the lengths
# the CPU received.
probe trap 0 igmp bytes
probe count 3 port 0 group "$GROUP_HEX" 32
probe count 4 port 0 group 0xe0000000 4
window trap "$CPU_S"
clear_probes

# A row for the group that names port 1 alone: does it steer the frames the
# host sends to the group away from the PON?
if [ "$ROW" = 1 ] && command -v igmpd >/dev/null; then
	probe count 3 port 0 group "$GROUP_HEX" 32
	probe count 4 port 0 group 0xe0000000 4
	if timeout "$IGMPD_S" igmpd -w -j "$GROUP" -p 0x2 4>&- > "$OUT/row.txt" 2>&1; then
		ROW_SET=1
		timeout "$DIAG_S" diag l2-table get all 4>&- | grep -i "^$GROUP_MAC" > "$OUT/row-table.txt"
		window row "$ROW_S"
		timeout "$IGMPD_S" igmpd -w -l "$GROUP" 4>&- >> "$OUT/row.txt" 2>&1 && ROW_SET=
	else
		say "igmpd -w -j failed: $(cat "$OUT/row.txt")"
	fi
	clear_probes
fi

snap end

# ---- the results -----------------------------------------------------------------

# line WINDOW SLOT TEXT: the count of the slot at the end of the window, the same
# counter read again a second later, and its rate.
line() {
	n=$(pv "$1.b" "$2" counted)
	m=$(pv "$1.c" "$2" counted)
	printf '  %-34s %10s  (%s/s; read again 1 s later: %s)\n' "$3" "${n:-?}" \
		"$(rate "${n:-?}" "$(secs "$1.a" "$1.b")")" "${m:-?}"
}

# mibline WINDOW PORT NAME TEXT
mibline() {
	d=$(mibd "$1.a" "$1.b" "$2" "$3")
	printf '  %-34s %10s  (%s/s)\n' "$4" "$d" "$(rate "$d" "$(secs "$1.a" "$1.b")")"
}

# cpuline WINDOW SLOT: what the CPU got from the rule of the slot.
cpuline() {
	printf '  %-34s %10s  frames, %s bytes, from ports %s; IGMP from the UNI by another reason: %s (last reason %s)\n' \
		"to the CPU, by the rule's reason" "$(pv "$1.b" "$2" cpu)" \
		"$(pv "$1.b" "$2" cpu_bytes)" "$(pv "$1.b" "$2" cpu_ports)" \
		"$(pv "$1.b" "$2" uni_igmp_other)" "$(pv "$1.b" "$2" other_reason)"
}

results() {
	echo "---- mcast-probe results: $LABEL, $(cat /etc/odi-build 2>/dev/null || echo '?'), group $GROUP ----"
	echo "count (frames, $(secs count.a count.b) s): which multicast enters each port"
	line count 0 "PON in, IGMP"
	line count 1 "PON in, other IPv4 multicast"
	line count 2 "UNI in, IGMP"
	line count 3 "UNI in, to $GROUP"
	line count 4 "UNI in, other IPv4 multicast"
	mibline count 2 ifInMulticastPkts "MIB port 2 in multicast"
	mibline count 0 ifOutMulticastPkts "MIB port 0 out multicast"
	mibline count 0 ifInMulticastPkts "MIB port 0 in multicast"
	mibline count 2 ifOutMulticastPkts "MIB port 2 out multicast"
	if [ -f "$OUT/bytes.b.probe" ]; then
		echo "bytes ($(secs bytes.a bytes.b) s): the same rules counting bytes; per frame from the count window's frame rate"
		for k in 0 1 2 3 4; do
			b=$(pv bytes.b "$k" counted)
			br=$(rate "${b:-?}" "$(secs bytes.a bytes.b)")
			fr=$(rate "$(pv count.b "$k" counted)" "$(secs count.a count.b)")
			printf '  slot %s %-27s %10s  (%s/s, about %s bytes a frame)\n' "$k" \
				"port $(pv bytes.b "$k" port) $(pv bytes.b "$k" dst) proto $(pv bytes.b "$k" proto)" \
				"${b:-?}" "$br" "$(per "$br" "$fr")"
		done
	fi
	echo "overlap ($(secs overlap.a overlap.b) s): two identical rules, PON in, 224.0.0.0/4"
	line overlap 0 "lower entry (88)"
	line overlap 1 "upper entry (89)"
	echo "mirror ($(secs mirror.a mirror.b) s): the UNI IGMP copied to the CPU"
	line mirror 0 "UNI in, IGMP, counted by the rule"
	cpuline mirror 0
	line mirror 3 "UNI in, to $GROUP"
	mibline mirror 2 ifOutMulticastPkts "MIB port 2 out multicast"
	echo "trap ($(secs trap.a trap.b) s): the UNI IGMP taken to the CPU, the rule counting bytes"
	line trap 0 "UNI in, IGMP, bytes (the rule)"
	cpuline trap 0
	line trap 3 "UNI in, to $GROUP"
	mibline trap 2 ifOutMulticastPkts "MIB port 2 out multicast"
	if [ -f "$OUT/row.b.probe" ]; then
		echo "row ($(secs row.a row.b) s): a static row for $GROUP naming port 1 alone"
		echo "  the row: $(cat "$OUT/row-table.txt" 2>/dev/null)"
		line row 3 "UNI in, to $GROUP"
		mibline row 2 ifOutMulticastPkts "MIB port 2 out multicast"
	fi
	echo "isolation, the whole run ($(secs start end) s):"
	printf '  %-34s %10s\n' "rx_pon_dropped (/proc/odi_nic)" "$(procd start end nic rx_pon_dropped)"
	printf '  %-34s %10s\n' "rx_pon (/proc/odi_nic)" "$(procd start end nic rx_pon)"
	printf '  %-34s %10s\n' "frames_delivered (/proc/odi_omci)" "$(procd start end omci frames_delivered)"
}

results | tee "$RES"
