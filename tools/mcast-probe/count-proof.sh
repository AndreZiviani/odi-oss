#!/bin/sh
#
# count-proof.sh -- prove, on a stick, that the multicast group counters
# igmpd meters its groups with (diag mc-count, odi_switch_mc_count.h) count
# what enters the PON port: the lost groups of ME 309, its alarm 0 and the
# bandwidth of ME 311 all rest on it, and until it is proved igmpd reports a
# group whose counter never moved as unknown, never lost (src/igmp/meter.h).
# Runs ON THE STICK, under busybox ash, on an image whose kernel and diag
# have the counters:
#
#     nohup sh /var/tmp/count-proof.sh > /var/tmp/count-proof.log 2>&1 &
#
# It counts the multicast that arrives on its own, on a line without IPTV:
# the IPv6 router advertisements and MLD queries to all-nodes
# (33:33:00:00:00:01), the rest of IPv6 multicast, IPv4 multicast (the
# OLT's or the router's queries to 224.0.0.x), broadcast; and compares each
# window with the port's own MIB (`ifInMulticastPkts`, `ifInBroadcastPkts`
# of port 2). Then the same on the UNI, with multicast the host sends, which
# checks the order of the MAC fields in the rule (an exact MAC rule for the
# test group against an IPv4 rule for the same group).
#
# One run is about 18 minutes (QUICK=1: about 3, too short for a router
# that advertises every few minutes). It ends with a results block, also in
# $OUT/results.txt, with one verdict line per question.
#
# What it changes, and puts back on every way out (the EXIT trap, and the
# kernel on top of it: the counters belong to one diag process fed from a
# FIFO here, and that process exiting, for whatever reason, removes them):
# ACL entries 48 to 50, template 1 and the units of log counter pairs 0 and
# 1, through diag mc-count: rules that count and change the forwarding of no
# frame. Nothing else: no flash, no config store, no boot slot variable, no
# service started or stopped, nothing sent, no register read directly. It
# refuses to run while igmpd runs (it would hold the counters) or while an
# ACL probe is in.
#
# The UNI windows need multicast from the host. Run this beside the script,
# on the host, on the interface that faces the stick (a Linux host; IFACE
# and the group as here; twenty 1000-byte pings to the group, a burst
# every 9 s or so):
#
#     end=$(( $(date +%s) + 1200 )); while [ "$(date +%s)" -lt "$end" ]; do
#       ping -q -c 20 -i 0.2 -s 1000 -t 1 -I IFACE 239.255.77.1; sleep 5; done
#
# Without it the PON windows still answer, and the UNI verdict says so.
#
# Settings, from the environment:
#   OUT        where the snapshots and results go (default /var/tmp/count-proof)
#   GROUP      the host's test group (default 239.255.77.1)
#   QUICK      1: every window 30 s
#   PON_S RA_S BYTES_S IP_S UNI_S   window lengths, seconds
#   LABEL      a word for the results header (the line, say ISP1)
#   DEV PROC   /dev/odi_sw and /proc, for test/count_proof_test.sh only
#
# POSIX sh; busybox timeout, mkfifo, pidof, awk.

PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH
set -u
trap '' HUP

OUT=${OUT:-/var/tmp/count-proof}
DEV=${DEV:-/dev/odi_sw}
PROC=${PROC:-/proc}
GROUP=${GROUP:-239.255.77.1}
LABEL=${LABEL:-?}
if [ "${QUICK:-0}" = 1 ]; then
	PON_S=${PON_S:-30} RA_S=${RA_S:-30} BYTES_S=${BYTES_S:-30}
	IP_S=${IP_S:-30} UNI_S=${UNI_S:-30}
else
	PON_S=${PON_S:-300} RA_S=${RA_S:-300} BYTES_S=${BYTES_S:-120}
	IP_S=${IP_S:-120} UNI_S=${UNI_S:-120}
fi

DIAG_S=10
POLL_TENTHS=50
FIFO=$OUT/ctl
RES=$OUT/results.txt
HOLDER=
OVERALL_S=$((PON_S + RA_S + BYTES_S + IP_S + 2 * UNI_S + 300))

exec 5>&1
say() { echo "count-proof: $*" >&5; }
die() { say "$*"; exit 1; }

diag1() { timeout "$DIAG_S" diag "$@" 4>&-; }
nap() {
	sleep "$1" 4>&- &
	wait $!
}

# ---- preconditions --------------------------------------------------------

mkdir -p "$OUT" || die "cannot create $OUT"
rm -f "$OUT"/*.mib "$OUT"/*.mcc "$OUT"/*.t "$RES" "$FIFO"
[ -c "$DEV" ] || die "no $DEV: not an odi-oss image"
command -v diag >/dev/null || die "no diag"
if pidof igmpd >/dev/null 2>&1; then
	die "igmpd is running: it holds the group counters; run this with IGMP_SNOOPING unset"
fi
diag1 mc-count show 0 > "$OUT/check.txt" 2>&1
grep -q '^slot=0 ' "$OUT/check.txt" ||
	die "this image has no group counters (diag mc-count show 0: $(cat "$OUT/check.txt"))"
grep -q ' empty$' "$OUT/check.txt" || die "a counter is already installed by another process"

case $GROUP in
*.*.*.*) ;;
*) die "GROUP must be a.b.c.d" ;;
esac
GROUP_HEX=$(echo "$GROUP" | awk -F. '{ printf "0x%02x%02x%02x%02x", $1, $2, $3, $4 }')
# The group's MAC as mc-count takes it: the first two octets, the last four.
GROUP_LO=$(echo "$GROUP" | awk -F. '{ printf "0x5e%02x%02x%02x", $2 % 128, $3, $4 }')

# ---- the holder -------------------------------------------------------------

start_holder() {
	mkfifo "$FIFO" || die "mkfifo $FIFO failed"
	timeout -s KILL "$OVERALL_S" diag < "$FIFO" > "$OUT/holder.out" 2>&1 &
	HOLDER=$!
	exec 4<>"$FIFO"
}

hold() { printf '%s\n' "$*" >&4; }

wait_slot() {
	t=0
	while [ "$t" -lt "$POLL_TENTHS" ]; do
		diag1 mc-count show "$1" > "$OUT/slot.txt" 2>&1
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

# count UNIT SLOT PORT ...: install through the holder and wait for it.
count() {
	hold "mc-count $*"
	wait_slot "$2" || {
		say "counter '$*' did not go in; the holder said: $(sed -n '$p' "$OUT/holder.out")"
		exit 1
	}
}

clear_counts() {
	hold "mc-count delete all"
	wait_slot 0 empty || die "the counters did not go"
}

# ---- snapshots ---------------------------------------------------------------

snap() {
	diag1 mib dump counter port 0,2 > "$OUT/$1.mib" 2>&1
	printf 'mc-count show %s\n' 0 1 2 | timeout "$DIAG_S" diag 4>&- |
		grep '^slot=' > "$OUT/$1.mcc"
	awk '{ print int($1) }' "$PROC/uptime" > "$OUT/$1.t"
}

mib() {
	awk -v p="$2" -v n="$3" '/^Port:/ { cur = $2 } cur == p && $1 == n { print $3; exit }' \
		"$OUT/$1.mib"
}

mibd() {
	a=$(mib "$1" "$3" "$4")
	b=$(mib "$2" "$3" "$4")
	if [ -n "$a" ] && [ -n "$b" ]; then echo $((b - a)); else echo '?'; fi
}

# cv LABEL SLOT KEY: one key of a counter line.
cv() {
	awk -v s="slot=$2" -v k="$3" '$1 == s {
		for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == k) { print kv[2]; exit } }
	}' "$OUT/$1.mcc"
}

secs() { echo $(($(cat "$OUT/$2.t") - $(cat "$OUT/$1.t"))); }

rate() {
	case $1 in
	*[!0-9]*|'') echo '?' ;;
	*) awk -v n="$1" -v s="$2" 'BEGIN { if (s > 0) printf "%.2f", n / s; else print "?" }' ;;
	esac
}

window() {
	say "window $1: $2 s"
	snap "$1.a"
	nap "$2"
	snap "$1.b"
	# Read again a second later with nothing changed: a counter that
	# cleared on read, or a total that did not fold, would read low.
	nap 1
	snap "$1.c"
}

# ---- the way out --------------------------------------------------------------

cleanup() {
	trap '' INT TERM
	if [ -n "$HOLDER" ]; then
		hold "mc-count delete all" 2>/dev/null
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
	printf 'mc-count show %s\n' 0 1 2 | timeout "$DIAG_S" diag |
		grep '^slot=' > "$OUT/left.mcc"
	left=$(grep -vc ' empty$' "$OUT/left.mcc")
	[ "$(grep -c '^slot=' "$OUT/left.mcc")" = 3 ] || left="? (the slots could not be read)"
	say "restored: counters left ${left:-?}"
	[ -f "$RES" ] && echo "restored: counters left ${left:-?}" >> "$RES"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---- the windows ---------------------------------------------------------------

start_holder

# PON, frames: every group-addressed frame, in three rules no two of which
# match one frame (of two that do, only the lower entry counts): IPv6
# multicast, every MAC whose first octet is 01 (IPv4 multicast, the 802.1
# groups), broadcast.
count frames 0 pon mac 0x3333 0 16
count frames 1 pon mac 0x0100 0 8
count frames 2 pon mac 0xffff 0xffffffff 48
window pon "$PON_S"
clear_counts

# PON, frames: all-nodes alone (router advertisements, MLD general
# queries), and the IPv4 link-local groups (224.0.0.x: IGMP queries).
count frames 0 pon mac 0x3333 0x00000001 48
count frames 1 pon mac 0x0100 0x5e000000 40
window ra "$RA_S"
clear_counts

# PON, bytes: the unit igmpd meters in.
count bytes 0 pon mac 0x3333 0 16
count bytes 1 pon mac 0x0100 0 8
window bytes "$BYTES_S"
clear_counts

# PON, an IPv4 rule for all of 224.0.0.0/4: the kind of rule the probes
# measured counting nothing on the PON.
count frames 0 pon ipv4 0xe0000000 4
window ip "$IP_S"
clear_counts

# UNI, frames: the host's test group by its exact MAC, the rest of IPv6
# multicast, broadcast.
count frames 0 uni mac 0x0100 "$GROUP_LO" 48
count frames 1 uni mac 0x3333 0 16
count frames 2 uni mac 0xffff 0xffffffff 48
window uni "$UNI_S"
clear_counts

# UNI, the same group by its IPv4 address: the same frames, by another match.
count frames 0 uni ipv4 "$GROUP_HEX" 32
window uni-ip "$UNI_S"
clear_counts

# ---- the results -----------------------------------------------------------------

# cnt WINDOW SLOT: what the slot counted in the window.
cnt() { v=$(cv "$1.b" "$2" counted); echo "${v:-?}"; }

line() {
	c=$(cnt "$1" "$2")
	again=$(cv "$1.c" "$2" counted)
	printf '  %-40s %10s  (%s/s; read again 1 s later: %s)\n' "$3" "$c" \
		"$(rate "$c" "$(secs "$1.a" "$1.b")")" "${again:-?}"
}

mibline() {
	d=$(mibd "$1.a" "$1.b" "$2" "$3")
	printf '  %-40s %10s  (%s/s)\n' "$4" "$d" "$(rate "$d" "$(secs "$1.a" "$1.b")")"
}

# sum A B ...: the sum, or ? when any is not a number.
sum() {
	s=0
	for v in "$@"; do
		case $v in *[!0-9]*|'') echo '?'; return ;; esac
		s=$((s + v))
	done
	echo "$s"
}

# verdict_count COUNTED EXPECTED: the counters against the MIB.
verdict_count() {
	case "$1$2" in *'?'*) echo "UNKNOWN (a reading is missing)"; return ;; esac
	if [ "$2" -lt 10 ]; then
		echo "INCONCLUSIVE (the port received only $2 such frames)"
	elif [ "$1" -eq 0 ]; then
		echo "FAIL (the rules counted nothing while the port received $2)"
	else
		awk -v c="$1" -v e="$2" 'BEGIN {
			d = c - e; if (d < 0) d = -d; tol = e / 20; if (tol < 3) tol = 3
			if (d <= tol) print "PASS (" c " counted, " e " received)"
			else print "MISMATCH (" c " counted, " e " received)"
		}'
	fi
}

results() {
	echo "---- count-proof results: $LABEL, $(cat /etc/odi-build 2>/dev/null || echo '?'), group $GROUP ----"
	echo "pon ($(secs pon.a pon.b) s, frames): every group-addressed frame entering the PON"
	line pon 0 "PON in, IPv6 multicast (33:33/16)"
	line pon 1 "PON in, other multicast (01/8)"
	line pon 2 "PON in, broadcast"
	mibline pon 2 ifInMulticastPkts "MIB port 2 in multicast"
	mibline pon 2 ifInBroadcastPkts "MIB port 2 in broadcast"
	mibline pon 0 ifOutMulticastPkts "MIB port 0 out multicast"
	echo "ra ($(secs ra.a ra.b) s, frames): all-nodes and the IPv4 link-local groups"
	line ra 0 "PON in, 33:33:00:00:00:01"
	line ra 1 "PON in, 01:00:5e:00:00:xx (224.0.0.x)"
	mibline ra 2 ifInMulticastPkts "MIB port 2 in multicast"
	echo "bytes ($(secs bytes.a bytes.b) s): the pon rules, counting bytes"
	line bytes 0 "PON in, IPv6 multicast, bytes"
	line bytes 1 "PON in, other multicast, bytes"
	echo "ip ($(secs ip.a ip.b) s, frames): an IPv4 rule, 224.0.0.0/4"
	line ip 0 "PON in, IPv4 224.0.0.0/4"
	echo "uni ($(secs uni.a uni.b) s, frames): what the host sends"
	line uni 0 "UNI in, $GROUP by its MAC"
	line uni 1 "UNI in, IPv6 multicast (33:33/16)"
	line uni 2 "UNI in, broadcast"
	mibline uni 0 ifInMulticastPkts "MIB port 0 in multicast"
	mibline uni 0 ifInBroadcastPkts "MIB port 0 in broadcast"
	echo "uni-ip ($(secs uni-ip.a uni-ip.b) s, frames): the same group by its IPv4 address"
	line uni-ip 0 "UNI in, $GROUP by its address"

	got=$(sum "$(cnt pon 0)" "$(cnt pon 1)" "$(cnt pon 2)")
	want=$(sum "$(mibd pon.a pon.b 2 ifInMulticastPkts)" "$(mibd pon.a pon.b 2 ifInBroadcastPkts)")
	echo "verdicts:"
	echo "  PON ingress counting (pon, rules against the MIB): $(verdict_count "$got" "$want")"
	ra=$(cnt ra 0)
	case $ra in
	*[!0-9]*|'') echo "  all-nodes on the PON (ra): UNKNOWN" ;;
	0) echo "  all-nodes on the PON (ra): none arrived in the window" ;;
	*) echo "  all-nodes on the PON (ra): PASS ($ra counted)" ;;
	esac
	b0=$(cnt bytes 0) b1=$(cnt bytes 1)
	echo "  bytes per frame on the PON (bytes / pon rates): 33:33 $(awk -v b="$b0" -v bs="$(secs bytes.a bytes.b)" -v f="$(cnt pon 0)" -v fs="$(secs pon.a pon.b)" 'BEGIN { if (b ~ /^[0-9]+$/ && f ~ /^[0-9]+$/ && f > 0 && bs > 0) printf "%.1f", (b / bs) / (f / fs); else print "?" }'), 01 $(awk -v b="$b1" -v bs="$(secs bytes.a bytes.b)" -v f="$(cnt pon 1)" -v fs="$(secs pon.a pon.b)" 'BEGIN { if (b ~ /^[0-9]+$/ && f ~ /^[0-9]+$/ && f > 0 && bs > 0) printf "%.1f", (b / bs) / (f / fs); else print "?" }') (64 to 1522 is a byte count)"
	u0=$(cnt uni 0) ui=$(cnt uni-ip 0)
	case "$u0$ui" in
	*[!0-9]*) echo "  MAC field order (uni against uni-ip): UNKNOWN" ;;
	*)
		if [ "$ui" -eq 0 ]; then
			echo "  MAC field order (uni against uni-ip): INCONCLUSIVE (no frame to $GROUP: is the host loop running?)"
		elif [ "$u0" -eq 0 ]; then
			echo "  MAC field order (uni against uni-ip): FAIL (the IPv4 rule counted $ui, the MAC rule nothing)"
		else
			awk -v m="$u0" -v ms="$(secs uni.a uni.b)" -v i="$ui" -v is="$(secs uni-ip.a uni-ip.b)" 'BEGIN {
				r = (m / ms) / (i / is)
				if (r > 0.8 && r < 1.25) printf "  MAC field order (uni against uni-ip): PASS (rates %.2f and %.2f a second)\n", m / ms, i / is
				else printf "  MAC field order (uni against uni-ip): MISMATCH (rates %.2f and %.2f a second)\n", m / ms, i / is
			}'
		fi
		;;
	esac
	echo "  forwarding (pon): multicast still left by the UNI with the rules in: $(mibd pon.a pon.b 0 ifOutMulticastPkts) frames (a rule only counts)"
}

results | tee "$RES"
