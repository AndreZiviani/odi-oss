#!/usr/bin/env bash
# count_proof_test.sh -- runs tools/mcast-probe/count-proof.sh against stub
# diag, timeout and pidof: the counters it installs, window by window, the
# results block and its verdicts, and that it puts everything back however
# it ends -- at the end of a run, on SIGTERM, and on SIGKILL, where only the
# end of input of the holder is left to do it (the stub diag drops the
# counters when its input ends or its holder is gone, as the kernel does
# when the file of the holder closes). Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=$ROOT/tools/mcast-probe/count-proof.sh
W=$(mktemp -d -t count_proof_test.XXXXXX)
trap '[ -n "${KEEP:-}" ] || rm -rf "$W"' EXIT
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

mkdir -p "$W/bin" "$W/proc"

# The stub diag: the counters are files in $FAKE_STATE, owned by the diag
# reading stdin that installed the first one. Each counts FAKE_RATE a
# second, or FAKE_ZERO_PON=1 nothing on the PON; the MIB of each port moves
# by what the stub's three PON or UNI rules would sum to.
cat > "$W/bin/diag" <<'STUB'
#!/bin/sh
S=$FAKE_STATE
gone() { rm -f "$S"/slot* "$S/owner"; }
held() { [ -f "$S/owner" ] && ! kill -0 "$(cat "$S/owner")" 2>/dev/null && gone; return 0; }
show() {
	if [ -f "$S/slot$1" ]; then
		read -r port since < "$S/slot$1"
		n=$(( ($(date +%s) - since) * ${FAKE_RATE:-2} ))
		[ "$port" = pon ] && [ "${FAKE_ZERO_PON:-0}" = 1 ] && n=0
		echo "slot=$1 entry=$((48 + $1)) counter=$1 port=2 match=mac dst=33:33:00:00:00:00/16 unit=frames counted=$n now=0x00000000"
	else
		echo "slot=$1 entry=$((48 + $1)) counter=$1 empty"
	fi
}
run() {
	# shellcheck disable=SC2086 # words, on purpose
	set -- $1
	case "$1 ${2:-}" in
	"mc-count show") held; show "$3" ;;
	"mc-count delete")
		if [ "$3" = all ]; then rm -f "$S"/slot*; else rm -f "$S/slot$3"; fi
		ls "$S"/slot* > /dev/null 2>&1 || rm -f "$S/owner"
		echo "delete $3" >> "$S/log" ;;
	"mc-count frames"|"mc-count bytes")
		if [ "$OWNER" != 1 ]; then echo "% busy"; return; fi
		[ -f "$S/slot$3" ] && { echo "% busy"; return; }
		echo "$$" > "$S/owner"
		echo "$4 $(date +%s)" > "$S/slot$3"
		echo "$*" >> "$S/log" ;;
	"mib dump")
		t=$(date +%s); echo "$t.00 1.00" > "$FAKE_PROC/uptime"
		for p in 0 2; do
			echo "Port: $p"
			echo "ifInMulticastPkts                  :                       $((t * 4))"
			echo "ifInBroadcastPkts                  :                       $((t * 2))"
			echo "ifOutMulticastPkts                 :                       $((t * 4))"
		done ;;
	"exit ") exit 0 ;;
	esac
}
if [ $# -gt 0 ]; then OWNER=0; run "$*"; exit 0; fi
OWNER=1
while read -r line; do echo "RTK.0> $line"; run "$line"; done
[ "$(cat "$S/owner" 2> /dev/null)" = "$$" ] && gone
exit 0
STUB
cat > "$W/bin/timeout" <<'STUB'
#!/bin/sh
[ "$1" = -s ] && shift 2
shift
exec "$@"
STUB
cat > "$W/bin/pidof" <<'STUB'
#!/bin/sh
[ -n "${FAKE_IGMPD:-}" ]
STUB
chmod +x "$W/bin/"*
echo "100.00 1.00" > "$W/proc/uptime"

setup() {
	rm -rf "$W/state" "$W/out"
	mkdir -p "$W/state"
	export FAKE_STATE=$W/state FAKE_PROC=$W/proc
	export PATH="$W/bin:$PATH" DEV=/dev/null PROC=$W/proc OUT=$W/out
	export PON_S=2 RA_S=1 BYTES_S=1 IP_S=1 UNI_S=1
}
left() { ls "$W/state"/slot* 2> /dev/null | wc -l | tr -d ' '; }

# 1. A whole run: three PON rules at 2 a second each against a MIB moving 6.
setup
if sh "$SCRIPT" > "$W/run.log" 2>&1; then ok "a run ends with status 0"; else bad "a run failed"; cat "$W/run.log"; fi
[ "$(left)" = 0 ] && ok "no counter is left" || bad "counters left after a run"
grep -q '^restored: counters left 0$' "$W/out/results.txt" && ok "the results say so" ||
	{ bad "no restored line"; cat "$W/out/results.txt"; }
for w in "pon (" "ra (" "bytes (" "ip (" "uni (" "uni-ip (" "verdicts:"; do
	grep -q "^$w" "$W/out/results.txt" && ok "results: $w" || bad "results lack: $w"
done
grep -q 'PON ingress counting.*: PASS' "$W/out/results.txt" && ok "counting the MIB's frames is a PASS" ||
	{ bad "no PASS for the PON"; cat "$W/out/results.txt"; }
grep -q 'MAC field order.*: PASS' "$W/out/results.txt" && ok "equal UNI rates are a PASS" ||
	bad "no PASS for the MAC order"
want='frames 0 pon mac 0x3333 0 16|frames 1 pon mac 0x0100 0 8|frames 2 pon mac 0xffff 0xffffffff 48|delete all|frames 0 pon mac 0x3333 0x00000001 48'
got=$(head -5 "$W/state/log" | sed 's/^mc-count //' | paste -sd'|' -)
[ "$got" = "$want" ] && ok "the pon window's rules, then the ra window's" || bad "rule order: $got"
grep -q '^frames 0 uni mac 0x0100 0x5e7f4d01 48$' <(sed 's/^mc-count //' "$W/state/log") &&
	ok "the test group's MAC, as two numbers" || bad "no exact MAC rule for the group"
grep -q '^frames 0 uni ipv4 0xefff4d01 32$' <(sed 's/^mc-count //' "$W/state/log") &&
	ok "and its address" || bad "no IPv4 rule for the group"

# 2. A PON whose rules count nothing while its MIB moves: a FAIL, not a PASS.
setup
FAKE_ZERO_PON=1 sh "$SCRIPT" > "$W/run.log" 2>&1 || true
grep -q 'PON ingress counting.*: FAIL' "$W/out/results.txt" && ok "nothing counted is a FAIL" ||
	{ bad "no FAIL for a PON that counts nothing"; cat "$W/out/results.txt"; }

# 3. igmpd running: refused before anything is installed.
setup
if FAKE_IGMPD=1 sh "$SCRIPT" > "$W/run.log" 2>&1; then bad "ran beside igmpd"; else ok "refuses beside igmpd"; fi
[ ! -f "$W/state/log" ] && ok "and installed nothing" || bad "installed a counter beside igmpd"

# 4. SIGTERM mid-window: the trap puts everything back.
setup
export PON_S=20
sh "$SCRIPT" > "$W/run.log" 2>&1 &
pid=$!
for _ in $(seq 50); do [ "$(left)" -ge 3 ] && break; sleep 0.1; done
[ "$(left)" -ge 3 ] && ok "three counters in for the pon window" || bad "the pon window never started"
kill -TERM "$pid"
wait "$pid" 2> /dev/null || true
[ "$(left)" = 0 ] && ok "SIGTERM: no counter is left" || bad "SIGTERM: counters left"
grep -q 'restored: counters left 0' "$W/run.log" && ok "SIGTERM: it says so" || bad "SIGTERM: no restored line"

# 5. SIGKILL mid-window: no trap runs; the holder input ends and the counters go.
setup
export PON_S=20
sh "$SCRIPT" > "$W/run.log" 2>&1 &
pid=$!
for _ in $(seq 50); do [ "$(left)" -ge 3 ] && break; sleep 0.1; done
{ kill -KILL "$pid"; wait "$pid"; } 2> /dev/null || true
for _ in $(seq 50); do [ "$(left)" = 0 ] && break; sleep 0.1; done
[ "$(left)" = 0 ] && ok "SIGKILL: the counters go with the holder" || bad "SIGKILL: counters left"

[ "$fail" = 0 ] && echo "count_proof_test: all checks passed"
exit "$fail"
