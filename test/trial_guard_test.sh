#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# rootfs/skeleton/etc/scripts/trial-guard.sh, off the device. The slot
# state file, /proc/uptime, /proc/net/tcp{,6} and the odi_wdt register are
# files under a temp dir; sleep advances the fake uptime instead of
# waiting; timeout records every bounded command it runs (the register
# writes among them) and runs it; ip prints what a fixture says. Checks
# when the guard arms, what proves the management path, and that it
# releases or leaves the client to the watchdog accordingly.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/trial-guard.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() {
	echo "FAIL  $1"
	for f in calls kmsg; do [ -f "$T/$f" ] && sed "s/^/        $f: /" "$T/$f"; done
	fail=$((fail + 1))
}

mkdir -p "$T/bin"
cat > "$T/bin/sleep" <<EOF
#!/bin/sh
u=\$(cut -d. -f1 "$T/uptime")
echo "\$((u + \$1)).00 0.00" > "$T/uptime"
n=\$(cat "$T/sleeps" 2>/dev/null || echo 0); n=\$((n + 1)); echo "\$n" > "$T/sleeps"
# commit_after: after that many sleeps, slot-state.sh says committed.
if [ -s "$T/commit_after" ] && [ "\$n" -ge "\$(cat "$T/commit_after")" ]; then
	sed 's/^uncommitted=1$/uncommitted=0/' "$T/state" > "$T/state.new" && mv "$T/state.new" "$T/state"
fi
# blank_after: after that many sleeps, the state cannot be told any more
# (slot-state.sh rewrote it while nv failed); remove_after: the file is gone.
if [ -s "$T/blank_after" ] && [ "\$n" -ge "\$(cat "$T/blank_after")" ]; then
	sed 's/^uncommitted=.*$/uncommitted=/' "$T/state" > "$T/state.new" && mv "$T/state.new" "$T/state"
fi
if [ -s "$T/remove_after" ] && [ "\$n" -ge "\$(cat "$T/remove_after")" ]; then
	rm -f "$T/state"
fi
exit 0
EOF
cat > "$T/bin/timeout" <<EOF
#!/bin/sh
shift
echo "\$*" >> "$T/calls"
exec "\$@"
EOF
cat > "$T/bin/ip" <<EOF
#!/bin/sh
[ "\$*" = "neigh show nud reachable" ] || exit 1
cat "$T/neigh" 2>/dev/null
exit 0
EOF
cat > "$T/bin/logger" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$T/bin/"*

TCP_HDR='  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode'
TCP6_HDR='  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode'
LISTEN22='   0: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1001 1 00000000 100 0 0 10 0'

# setup <state lines...>: a fresh run directory; the state file holds the
# lines given (none: no state file at all).
setup() {
	rm -f "$T/calls" "$T/kmsg" "$T/neigh" "$T/sleeps" "$T/commit_after" "$T/blank_after" "$T/remove_after" "$T/state" "$T/off" "$T/prev"
	echo "40.00 0.00" > "$T/uptime"
	: > "$T/register"
	printf '%s\n%s\n' "$TCP_HDR" "$LISTEN22" > "$T/tcp"
	printf '%s\n' "$TCP6_HDR" > "$T/tcp6"
	if [ $# -gt 0 ]; then printf '%s\n' "$@" > "$T/state"; fi
}
trial() { setup running=1 sw_tryactive=2 primary_sw_commit=0 next_boot=0 uncommitted=1; }
tcp() { echo "   1: $1 $2 $3 00000000:00000000 00:00000000 00000000     0        0 2001 1 00000000 20 4 30 10 -1" >> "$T/tcp"; }
tcp6() { echo "   1: $1 $2 $3 00000000:00000000 00:00000000 00000000     0        0 2002 1 00000000 20 4 30 10 -1" >> "$T/tcp6"; }

run() {
	PATH="$T/bin:$PATH" TRIAL_GUARD_PATH="$T/bin" STATE="$T/state" REGISTER="${REG:-$T/register}" OFF_FLAG="$T/off" \
		PROC_TCP="$T/tcp $T/tcp6" UPTIME="$T/uptime" PREV="$T/prev" KMSG="$T/kmsg" \
		IP=ip LOGGER=logger SLEEP=sleep TIMEOUT=timeout \
		STATE_WAIT_S=3 sh "$S"
	echo "$?" > "$T/rc"
}

armed()    { grep -q -- '_ trial 300 ' "$T/calls" 2>/dev/null; }
released() { grep -q -- '_ trial 0 ' "$T/calls" 2>/dev/null; }
kmsg()     { grep -q -- "$1" "$T/kmsg" 2>/dev/null; }
rc0()      { [ "$(cat "$T/rc")" = 0 ]; }

# expect <name> <armed 0|1> <released 0|1> [kmsg pattern]
expect() {
	local name=$1 a=$2 r=$3 pat=${4:-}
	local got_a=0 got_r=0
	armed && got_a=1
	released && got_r=1
	if [ "$got_a" = "$a" ] && [ "$got_r" = "$r" ] && rc0 && { [ -z "$pat" ] || kmsg "$pat"; }; then
		ok "$name"
	else
		no "$name (armed $got_a want $a, released $got_r want $r, rc $(cat "$T/rc")${pat:+, kmsg /$pat/})"
	fi
}

# ---- when it arms --------------------------------------------------------
setup running=1 sw_tryactive=2 primary_sw_commit=1 next_boot=1 uncommitted=0
run; expect "committed slot: not armed" 0 0
setup running=0 primary_sw_commit=0 fallback_sw_commit=1 next_boot=0 uncommitted=1
run; expect "half committed (a reboot boots this slot again): not armed" 0 0
setup running= next_boot= uncommitted=
run; expect "no running slot: not armed" 0 0
# nv could not read the environment, the running slot is known: it may be
# a trial, so armed, once. After a boot the guard ended, not again.
setup running=1 next_boot= uncommitted=
echo "previous boot: boot=7 slot=1 build=x crumb=svc/3 reason=power" > "$T/prev"
run; expect "slot known, state unknown: armed" 1 0 "the watchdog resets slot 1, whose state is unknown"
setup running=1 next_boot= uncommitted=
echo "previous boot: boot=7 slot=1 build=x crumb=svc/3 reason=wdt_client:trial" > "$T/prev"
run; expect "slot known, state unknown, last boot ended by the guard: not armed again" 0 0 "not armed again"
setup running=0 next_boot= uncommitted=
echo "previous boot: boot=7 slot=1 build=x crumb=svc/3 reason=wdt_client:omcid" > "$T/prev"
run; expect "slot known, state unknown, another client ended the last boot: armed" 1 0
setup running=1 next_boot= uncommitted=
tcp C0A80101:0016 C0A80102:D431 01
run; expect "slot known, state unknown, an ssh session: released" 1 1 "tcp peer C0A80102 state 01"
setup
run; expect "no state file within the wait: not armed" 0 0
trial; : > "$T/off"
run; expect "trial-guard.off: not armed" 0 0
trial; REG=$T/no-such-register
run; expect "no register (an older kernel): not armed, said so" 0 0 "not armed"
unset REG

# ---- unproven: armed and left to the watchdog -------------------------------
trial
run; expect "trial, nobody talks: armed, never released" 1 0 "the watchdog reverts this trial to slot 0"
u=$(cut -d. -f1 "$T/uptime")
if [ "$u" -ge 340 ] && [ "$u" -le 350 ]; then ok "it watches for the whole deadline (uptime $u)"; else no "watched until uptime $u, wanted about 340"; fi
trial
tcp 7F000001:0016 7F000001:A000 01		# ssh to 127.0.0.1
tcp C0A80101:1F90 C0A80101:B000 01		# the stick to its own address
tcp C0A80101:0016 C0A80102:C000 03		# SYN_RECV: our SYN-ACK unanswered
tcp C0A80101:D000 C0A80102:0050 02		# SYN_SENT
tcp6 00000000000000000000000000000001:0016 00000000000000000000000000000001:A001 01
tcp6 00000000000000000000FFFF7F000001:0016 00000000000000000000FFFF7F000001:A002 01
run; expect "loopback, self, SYN_RECV and SYN_SENT prove nothing" 1 0
# Two different loopback addresses: only the loopback match skips these,
# the same-address check does not.
trial
tcp 7F000001:0016 7F000002:A003 01		# 127.0.0.1 to 127.0.0.2
tcp6 00000000000000000000FFFF7F000001:0016 00000000000000000000FFFF7F000102:A004 01
run; expect "two different 127/8 addresses prove nothing" 1 0

# An armed trial keeps its deadline when the state stops being readable:
# only uncommitted=0 is a commit.
trial; echo 3 > "$T/blank_after"
run; expect "state unreadable during the trial: stays armed" 1 0 "the watchdog reverts this trial to slot 0"
trial; echo 3 > "$T/remove_after"
run; expect "state file gone during the trial: stays armed" 1 0 "the watchdog reverts this trial to slot 0"

# ---- proven -----------------------------------------------------------------
trial; tcp C0A80101:0016 C0A80102:C000 01
run; expect "an ssh session from the host: released" 1 1 "tcp peer C0A80102 state 01"
trial; tcp C0A80101:0016 C0A8017F:C000 01		# a host at 192.168.1.127
run; expect "a host whose address ends in .127: released" 1 1 "tcp peer C0A8017F state 01"
trial; tcp C0A80101:2386 C0A80102:C100 06
run; expect "a finished scrape in TIME_WAIT: released" 1 1 "state 06"
trial; tcp6 FE80000000000000000000FFFE000002:0016 FE80000000000000000000FFFE000003:C200 01
run; expect "an IPv6 session: released" 1 1 "tcp peer FE800000"
trial; echo "192.168.1.2 dev br0 lladdr 02:00:00:00:00:02 REACHABLE" > "$T/neigh"
run; expect "a reachable neighbour: released" 1 1 "reachable neighbour 192.168.1.2"
trial; echo 3 > "$T/commit_after"
run; expect "committed by hand during the trial: released" 1 1 "is committed now"
n=$(cat "$T/sleeps")
if [ "$n" -le 4 ]; then ok "and promptly ($n polls)"; else no "released after $n polls"; fi

echo "trial_guard_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
