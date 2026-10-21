#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/mgmt-addr.sh, off the device, on top of the real
# network.sh and the real flash.
#
# ifconfig is a stub that keeps one file per interface or alias under $T/if,
# holding "ADDR MASK" (as network_addr_test.sh does), /proc/uptime is a file
# the sleep stub advances, and setsid only records what would be spawned: each
# test runs the spawned verb itself, so the order of events is the test, not
# the scheduler. The config store is a temporary lastgood.xml, written by the
# real flash. A reboot is a wipe of /var/run (the state and the trial file) and
# of the interfaces, then the addresses the store says.
#
# What is checked: a trial applies the new address and keeps the old one on
# br0:4, a confirm over the new address stores the keys, an expiry and a
# revert put everything back and store nothing, a reboot in the middle of a
# trial comes up on the old address with the store untouched, a confirm over
# the old address is refused, the same for a second-address-only and a
# netmask-only change, every refusal of try, the SSH_CONNECTION fallback, a
# key that does not stick, and the stale lock.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/mgmt-addr.sh
N=$PWD/rootfs/skeleton/etc/scripts/network.sh
F=$PWD/rootfs/skeleton/etc/scripts/flash
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
# The test may itself run over ssh (a remote build): an inherited
# SSH_CONNECTION would answer the confirm that the cases below expect to
# carry no address, so only the cases that set it see one.
unset SSH_CONNECTION

pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
# t NAME PATTERN TEXT: TEXT matches the extended regex PATTERN.
t() { if [[ $3 =~ $2 ]]; then ok "$1"; else no "$1" "wanted /$2/, got: $3"; fi; }
# n NAME PATTERN TEXT: TEXT does not match.
n() { if [[ $3 =~ $2 ]]; then no "$1" "did not want /$2/, got: $3"; else ok "$1"; fi; }

mkdir -p "$T/bin" "$T/config" "$T/sys/br0"
cat > "$T/bin/ifconfig" <<'STUB'
#!/bin/sh
f="$IFSTATE/$1"
case "${2:-}" in
"")   [ -f "$f" ] || { echo "ifconfig: $1: error fetching interface information" >&2; exit 1; }
      read -r a m < "$f"
      printf '%s  Link encap:Ethernet\n          inet addr:%s  Bcast:0.0.0.0  Mask:%s\n' "$1" "$a" "$m" ;;
down) rm -f "$f" ;;
*)    echo "$2 $4" > "$f" ;;
esac
STUB
cat > "$T/bin/timeout" <<'EOF'
#!/bin/sh
shift
exec "$@"
EOF
cat > "$T/bin/setsid" <<EOF
#!/bin/sh
shift 2
echo "\$*" >> "$T/spawned"
EOF
# sleep N: N seconds pass. A hook, when present, runs once inside a sleep
# longer than a second: the trial window, where a snapshot, a confirm, a
# revert or a reboot would happen.
cat > "$T/bin/sleep" <<EOF
#!/bin/sh
up=\$(cut -d. -f1 "$T/uptime")
echo "\$((up + \$1)).00 1.00" > "$T/uptime"
[ -x "$T/hook" ] && [ "\$1" -gt 1 ] && { "$T/hook"; rm -f "$T/hook"; }
exit 0
EOF
chmod +x "$T/bin/"*

export PATH="$T/bin:$PATH"
export FLASH=$F NETWORK=$N SELF=$S STATE=$T/run/state TRIAL=$T/run/trial LOCKDIR=$T/run/lock
export SAFE_FLAG=$T/run/safe-mode OVERRIDE=$T/lan-ip
export UPTIME=$T/uptime KMSG=$T/kmsg LOG=$T/services.log SLEEP=$T/bin/sleep TIMEOUT=$T/bin/timeout
export IFSTATE=$T/if IFCONFIG=$T/bin/ifconfig SYSNET=$T/sys
export CS=$T/config/lastgood.xml HS=$T/config/lastgood_hs.xml CONF=$T/config/lastgood.xml HSCONF=$T/config/lastgood_hs.xml
export ODI_CONF=$T/config/odi.conf TMPDIR_=$T
export TRIAL_S=30 T
echo '<Config Name="ROOT"/>' > "$HS"

st() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null; }
iface() { cat "$T/if/$1" 2>/dev/null || echo none; }
key() { sed -n "s/.*Name=\"$1\" Value=\"\([^\"]*\)\".*/\1/p" "$CS" | head -n 1; }

# store IP MASK ENABLE2 IP2 MASK2: the five keys, in a fresh lastgood.xml.
store() {
	cat > "$CS" <<XML
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="LAN_IP_ADDR" Value="$1"/>
		<Value Name="LAN_SUBNET" Value="${2:-255.255.255.0}"/>
		<Value Name="LAN_ENABLE_IP2" Value="${3:-0}"/>
		<Value Name="LAN_IP_ADDR2" Value="${4:-192.168.100.1}"/>
		<Value Name="LAN_SUBNET2" Value="${5:-255.255.255.0}"/>
	</Dir>
</Config>
XML
}
# The interfaces as a boot leaves them: the store addresses, nothing else.
boot_ifaces() {
	rm -rf "$T/if"; mkdir -p "$T/if"
	echo "$(key LAN_IP_ADDR) $(key LAN_SUBNET)" > "$T/if/br0"
	[ "$(key LAN_ENABLE_IP2)" = 1 ] && echo "$(key LAN_IP_ADDR2) $(key LAN_SUBNET2)" > "$T/if/br0:2"
	return 0
}
# A fresh boot: /var/run is a tmpfs, the uptime starts again, the interfaces
# hold what the store says.
newboot() {
	rm -rf "$T/run" "$T/spawned" "$T/hook" "$T/lan-ip" "$T/kmsg"
	mkdir -p "$T/run"
	echo "15.00 1.00" > "$T/uptime"
	: > "$T/kmsg"
	boot_ifaces
}
hook() { printf '#!/bin/sh\n%s\n' "$1" > "$T/hook"; chmod +x "$T/hook"; }
# snapshot of the interfaces inside the window, to $T/snap.*
SNAP='for i in br0 br0:2 br0:4; do cat "$IFSTATE/$i" > "$T/snap.$i" 2>/dev/null || echo none > "$T/snap.$i"; done'
snap() { cat "$T/snap.$1" 2>/dev/null; }
spawned() {
	_i=0
	while [ ! -s "$T/spawned" ] && [ "$_i" -lt 100 ]; do /bin/sleep 0.05; _i=$((_i + 1)); done
	cat "$T/spawned" 2>/dev/null
}
spawned_none() { /bin/sleep 0.3; cat "$T/spawned" 2>/dev/null; }
# run_trial: the spawned trial-run, itself, with the hook in its window.
run_trial() { sh "$S" trial-run "$(st trial_id)" 30 > /dev/null 2>&1; }

OLD="192.168.0.1 255.255.255.0"
NEW="10.1.1.1 255.255.255.0"

# ---- try: the new address is planned, nothing applied, nothing stored
store 192.168.0.1 255.255.255.0 0 192.168.100.1
newboot
out=$(sh "$S" try LAN_IP_ADDR=10.1.1.1 2>&1); rc=$?
t "try: started, naming the address to confirm on" "event=trial_started .*trying for 30 s: confirm over 10\.1\.1\.1 " "$out"
t "try: says what network.sh would do" "would move br0 to 10.1.1.1 netmask 255.255.255.0 \(was 192.168.0.1" "$out"
[ "$rc" = 0 ] && ok "try: exit 0" || no "try: exit 0" "rc $rc"
t "try: phase trial, not applied, deadline uptime+31" "^trial 0 46$" "$(st phase) $(st applied) $(st trial_deadline)"
t "try: the previous primary is recorded" "^192.168.0.1 255.255.255.0$" "$(st prev_ip) $(st prev_mask)"
t "try: the answer leaves first, nothing is applied yet" "^$OLD$" "$(iface br0)"
t "try: the store is untouched" "^192.168.0.1$" "$(key LAN_IP_ADDR)"
t "try: the trial file is in /var/run, not in the store" "^LAN_IP_ADDR=10.1.1.1$" "$(sed -n 1p "$TRIAL")"
t "try: trial-run spawned with its id" "^trial-run $(st trial_id) 30$" "$(spawned)"
t "try: logged to the kernel" "event=trial_started ip=10.1.1.1 prev=192.168.0.1" "$(cat "$T/kmsg")"
out=$(sh "$S" confirm 10.1.1.1 192.168.0.50 2>&1); rc=$?
t "confirm before the apply is refused" "not applied yet" "$out"
[ "$rc" = 1 ] && ok "  ... with exit 1" || no "  ... with exit 1" "rc $rc"
out=$(sh "$S" try LAN_IP_ADDR=10.2.2.2 2>&1)
t "a second trial is refused" "already running" "$out"

# ---- the window, then the expiry: both addresses answer, then the old one only
hook "$SNAP"
run_trial
t "window: the primary is the new address" "^$NEW$" "$(snap br0)"
t "window: the old primary stays up on br0:4" "^$OLD$" "$(snap br0:4)"
t "expiry: back to the old primary" "^$OLD$" "$(iface br0)"
t "expiry: the kept address is taken down" "^none$" "$(iface br0:4)"
t "expiry: state steady, event trial_expired" "^steady trial_expired$" "$(st phase) $(st event)"
t "expiry: nothing was stored" "^192.168.0.1$" "$(key LAN_IP_ADDR)"
t "expiry: the trial file is gone" "^absent$" "$([ -e "$TRIAL" ] && echo present || echo absent)"
t "expiry: logged" "event=trial_expired .*not confirmed in time" "$(cat "$T/kmsg")"
out=$(sh "$S" confirm 10.1.1.1 192.168.0.50 2>&1); rc=$?
t "confirm after the expiry is refused" "no trial is running" "$out"
[ "$rc" = 1 ] && ok "  ... with exit 1" || no "  ... with exit 1" "rc $rc"

# ---- a confirm over the NEW address stores the keys
newboot; : > "$T/spawned"
sh "$S" try LAN_IP_ADDR=10.1.1.1 > /dev/null
hook "$SNAP; sh '$S' confirm 10.1.1.1 10.1.1.50 > '$T/confirm.out' 2>&1"
run_trial
t "confirm: the window held both addresses" "^$NEW $OLD$" "$(snap br0) $(snap br0:4)"
t "confirm: the new primary is stored" "^10.1.1.1$" "$(key LAN_IP_ADDR)"
t "confirm: the netmask is untouched" "^255.255.255.0$" "$(key LAN_SUBNET)"
t "confirm: state steady, event trial_confirmed" "^steady trial_confirmed$" "$(st phase) $(st event)"
t "confirm: the primary stays on the new address" "^$NEW$" "$(iface br0)"
t "confirm: the old address is taken down" "^none$" "$(iface br0:4)"
t "confirm: the trial file is gone" "^absent$" "$([ -e "$TRIAL" ] && echo present || echo absent)"
t "confirm: the expiry after it changed nothing" "^$NEW$" "$(iface br0)"
t "confirm: logged with the address it arrived on" "event=trial_confirmed .*confirmed on 10\.1\.1\.1 and stored: LAN_IP_ADDR" "$(cat "$T/kmsg")"
# and a boot after it comes up on the new address
rm -rf "$T/run"; mkdir -p "$T/run"; boot_ifaces
t "confirm: the next boot reads the new address from the store" "^$NEW$" "$(iface br0)"

# ---- a confirm over the OLD address is refused; the trial carries on
store 192.168.0.1 255.255.255.0 0 192.168.100.1
newboot
sh "$S" try LAN_IP_ADDR=10.1.1.1 > /dev/null
hook "sh '$S' confirm 192.168.0.1 192.168.0.50 > '$T/confirm.out' 2>&1; echo \$? > '$T/confirm.rc'; $SNAP; sh '$S' status > '$T/status.out'"
run_trial
t "old address: refused, saying which one to use" "not confirmed: it arrived on 192\.168\.0\.1, which the stick already had\. Confirm on the new address: 10\.1\.1\.1" "$(cat "$T/confirm.out")"
t "old address: exit 1" "^1$" "$(cat "$T/confirm.rc")"
t "old address: the trial was still running afterwards" "phase=trial" "$(cat "$T/status.out")"
t "old address: nothing stored" "^192.168.0.1$" "$(key LAN_IP_ADDR)"
t "old address: it reverted at the end of the window" "^trial_expired $OLD$" "$(st event) $(iface br0)"

# the same from a client address that is not ours at all, and with no address
newboot
sh "$S" try LAN_IP_ADDR=10.1.1.1 > /dev/null
hook "sh '$S' confirm 172.16.0.9 172.16.0.50 > '$T/confirm.out' 2>&1; sh '$S' confirm > '$T/confirm2.out' 2>&1"
run_trial
t "an unrelated address is refused too" "not confirmed: it arrived on 172\.16\.0\.9" "$(cat "$T/confirm.out")"
t "no address at all is refused" "cannot be told which address" "$(cat "$T/confirm2.out")"

# ---- SSH_CONNECTION: peer, peer port, local, local port
newboot
sh "$S" try LAN_IP_ADDR=10.1.1.1 > /dev/null
hook "SSH_CONNECTION='192.168.0.50 40000 192.168.0.1 22' sh '$S' confirm > '$T/confirm.out' 2>&1; SSH_CONNECTION='10.1.1.50 40001 10.1.1.1 22' sh '$S' confirm >> '$T/confirm.out' 2>&1"
run_trial
t "over ssh to the old address: refused" "it arrived on 192\.168\.0\.1" "$(cat "$T/confirm.out")"
t "over ssh to the new address: confirmed" "^trial_confirmed$" "$(st event)"
t "over ssh to the new address: stored" "^10.1.1.1$" "$(key LAN_IP_ADDR)"

# ---- a reboot in the middle of the trial: the old address, the store untouched
store 192.168.0.1 255.255.255.0 0 192.168.100.1
newboot
sh "$S" try LAN_IP_ADDR=10.1.1.1 LAN_SUBNET=255.255.0.0 > /dev/null
hook "$SNAP; rm -rf '$T/run'; mkdir -p '$T/run'; echo '15.00 1.00' > '$T/uptime'"
run_trial
t "reboot mid-trial: the trial had applied" "^10.1.1.1 255.255.0.0$" "$(snap br0)"
# The sleeper dies with the reboot; in the test it ran on and found no trial.
boot_ifaces
t "reboot mid-trial: the boot comes up on the old address" "^$OLD$" "$(iface br0)"
t "reboot mid-trial: the keys were never stored" "^192.168.0.1 255.255.255.0$" "$(key LAN_IP_ADDR) $(key LAN_SUBNET)"
t "reboot mid-trial: no state, no trial file" "^absent absent$" "$([ -e "$STATE" ] && echo present || echo absent) $([ -e "$TRIAL" ] && echo present || echo absent)"
t "reboot mid-trial: status says steady" "phase=steady" "$(sh "$S" status)"
sh "$N" addr > /dev/null
t "reboot mid-trial: network.sh addr after it changes nothing" "^$OLD none$" "$(iface br0) $(iface br0:4)"
# A trial file left behind (a tmpfs that survived) is read by `addr` only: the
# boot and the watchdog check read the store.
printf 'LAN_IP_ADDR=10.9.9.9\nLAN_SUBNET=255.255.255.0\nLAN_ENABLE_IP2=0\nLAN_IP_ADDR2=\nLAN_SUBNET2=\nKEEP_IP=\nKEEP_MASK=\n' > "$TRIAL"
t "a stale trial file does not touch the watchdog check" "holds the configured address 192.168.0.1" "$(sh "$N" configured 2>&1)"
rm -f "$TRIAL"

# ---- revert: the answer leaves first, the move back is one second later
newboot
sh "$S" try LAN_IP_ADDR=10.1.1.1 > /dev/null
hook "sh '$S' revert > '$T/revert.out' 2>&1; $SNAP"
: > "$T/spawned"
run_trial
t "revert: reported" "event=trial_reverted .*reverted on request" "$(cat "$T/revert.out")"
t "revert: the answer left before anything moved" "^$NEW$" "$(snap br0)"
_i=0; while ! grep -q '^settle$' "$T/spawned" 2>/dev/null && [ "$_i" -lt 100 ]; do /bin/sleep 0.05; _i=$((_i + 1)); done
t "revert: the move back is spawned" "settle" "$(cat "$T/spawned")"
t "revert: the trial file is gone at once" "^absent$" "$([ -e "$TRIAL" ] && echo present || echo absent)"
t "revert: state steady, nothing stored" "^steady trial_reverted 192.168.0.1$" "$(st phase) $(st event) $(key LAN_IP_ADDR)"
sh "$S" settle > /dev/null
t "revert: settle moves the primary back and drops br0:4" "^$OLD none$" "$(iface br0) $(iface br0:4)"
out=$(sh "$S" revert 2>&1)
t "revert with no trial is refused" "no trial is running" "$out"

# ---- the second address on its own
store 192.168.0.1 255.255.255.0 0 192.168.100.1
newboot
out=$(sh "$S" try LAN_ENABLE_IP2=1 LAN_IP_ADDR2=172.20.0.1 2>&1)
t "second address: confirm over it" "confirm over 172\.20\.0\.1 " "$out"
hook "$SNAP; sh '$S' confirm 192.168.0.1 192.168.0.50 > '$T/confirm.out' 2>&1; sh '$S' confirm 172.20.0.1 172.20.0.50 >> '$T/confirm.out' 2>&1"
run_trial
t "second address: the primary stayed, the second came up on br0:2" "^$OLD 172.20.0.1 255.255.255.0$" "$(snap br0) $(snap br0:2)"
t "second address: no br0:4, the primary did not move" "^none$" "$(snap br0:4)"
t "second address: a confirm on the primary is refused" "it arrived on 192\.168\.0\.1" "$(cat "$T/confirm.out")"
t "second address: a confirm on the new one stores the three keys" "^1 172.20.0.1$" "$(key LAN_ENABLE_IP2) $(key LAN_IP_ADDR2)"

# the primary moves onto the stored second address: it is already reachable
store 192.168.0.1 255.255.255.0 1 172.20.0.1
newboot
sh "$S" try LAN_IP_ADDR=172.20.0.1 LAN_IP_ADDR2=192.168.0.1 > /dev/null
hook "$SNAP"
run_trial
t "swap: nothing is kept on br0:4 when the old primary is the new second" "^172.20.0.1 255.255.255.0 192.168.0.1 255.255.255.0 none$" "$(snap br0) $(snap br0:2) $(snap br0:4)"

# ---- the netmask on its own: nothing new to reach, the client must be on-link
store 192.168.0.1 255.255.255.0 0 192.168.100.1
newboot
out=$(sh "$S" try LAN_SUBNET=255.255.0.0 2>&1)
t "netmask only: asks for a client on the new netmask" "confirm over a client on the new netmask" "$out"
hook "$SNAP; sh '$S' confirm 192.168.0.1 10.0.0.5 > '$T/confirm.out' 2>&1; sh '$S' confirm 192.168.0.1 192.168.77.5 >> '$T/confirm.out' 2>&1"
run_trial
t "netmask only: the mask moved, no alias" "^192.168.0.1 255.255.0.0 none$" "$(snap br0) $(snap br0:4)"
t "netmask only: a client off the new link is refused" "only the netmask moved: confirm from a client on the new netmask \(255\.255\.0\.0\), not from 10\.0\.0\.5" "$(cat "$T/confirm.out")"
t "netmask only: a client on it confirms and the mask is stored" "^255.255.0.0$" "$(key LAN_SUBNET)"

# ---- try refusals
store 192.168.0.1 255.255.255.0 0 192.168.100.1
newboot
for bad in 0.0.0.0 127.0.0.1 224.0.0.1 192.168.010.1 192.168.1 192.168.0.255; do
	out=$(sh "$S" try LAN_IP_ADDR="$bad" 2>&1); rc=$?
	t "try refuses an address network.sh refuses: $bad" "network.sh refuses these values" "$out"
	[ "$rc" = 1 ] && [ ! -e "$STATE" ] && [ ! -e "$TRIAL" ] && ok "  ... exit 1, no state, no trial file" || no "  ... exit 1, no state, no trial file" "rc $rc"
done
out=$(sh "$S" try LAN_SUBNET=255.255.255.1 2>&1)
t "try refuses a netmask network.sh refuses" "network.sh refuses these values" "$out"
t "none of them touched the interfaces or the store" "^$OLD 192.168.0.1$" "$(iface br0) $(key LAN_IP_ADDR)"
out=$(sh "$S" try LAN_IP_ADDR= 2>&1)
t "try refuses an empty primary" "LAN_IP_ADDR cannot be empty" "$out"
out=$(sh "$S" try LAN_FOO=1 2>&1)
t "try refuses a key that is not an address key" "LAN_FOO is not a management address key" "$out"
out=$(sh "$S" try HOST_AUTONEG=off 2>&1)
t "  ... the host port key included" "HOST_AUTONEG is not a management address key" "$out"
out=$(sh "$S" try 10.1.1.1 2>&1)
t "try refuses a bare value" "is not KEY=VALUE" "$out"
out=$(sh "$S" try 2>&1)
t "try with nothing to try is a usage error" "usage" "$out"
out=$(sh "$S" try -s 5 LAN_IP_ADDR=10.1.1.1 2>&1)
t "try with too short a window is refused" "SECONDS must be 10 to 600" "$out"
out=$(sh "$S" try -s 9999 LAN_IP_ADDR=10.1.1.1 2>&1)
t "try with too long a window is refused" "SECONDS must be 10 to 600" "$out"
out=$(sh "$S" try -s abc LAN_IP_ADDR=10.1.1.1 2>&1)
t "try with a window that is no number is refused" "SECONDS must be a number" "$out"
out=$(sh "$S" try LAN_IP_ADDR=192.168.0.1 2>&1); rc=$?
t "try of the stored values has nothing to do" "nothing to try" "$out"
[ "$rc" = 0 ] && [ ! -e "$STATE" ] && ok "  ... exit 0, no state" || no "  ... exit 0, no state" "rc $rc"
echo "10.0.0.5" > "$OVERRIDE"
out=$(sh "$S" try LAN_IP_ADDR=10.1.1.1 2>&1)
t "try refuses while the lan-ip override exists" "wins over LAN_IP_ADDR" "$out"
out=$(sh "$S" try LAN_ENABLE_IP2=1 LAN_IP_ADDR2=172.20.0.1 2>&1)
t "  ... but the second address is not covered by it" "trial_started" "$out"
sh "$S" revert > /dev/null; rm -f "$OVERRIDE"; sh "$S" settle > /dev/null

# ---- safe mode: no trial, the rescue address is the way in
newboot
echo "short_boots=5" > "$SAFE_FLAG"
out=$(sh "$S" try LAN_IP_ADDR=10.1.1.1 2>&1); rc=$?
t "safe-mode boot: a trial is refused" "safe-mode boot: no trial, the rescue address" "$out"
[ "$rc" = 1 ] && [ ! -e "$TRIAL" ] && ok "  ... exit 1, nothing applied" || no "  ... exit 1, nothing applied" "rc $rc"
t "status says so" "safe_mode_boot=1" "$(sh "$S" status)"
# network.sh addr in a safe-mode boot still adds the rescue address beside a trial
rm -f "$SAFE_FLAG"

# ---- a key that does not stick: nothing is kept, what was written is put back
newboot
grep -v 'LAN_SUBNET2' "$CS" > "$CS.new" && mv "$CS.new" "$CS"
sh "$S" try LAN_IP_ADDR=10.1.1.1 LAN_SUBNET2=255.255.0.0 > /dev/null
hook "sh '$S' confirm 10.1.1.1 10.1.1.50 > '$T/confirm.out' 2>&1; echo \$? > '$T/confirm.rc'; sh '$S' status > '$T/status.out'"
run_trial
t "a key that does not stick: refused, naming it" "could not store LAN_SUBNET2=255.255.0.0" "$(cat "$T/confirm.out")"
t "  ... exit 1" "^1$" "$(cat "$T/confirm.rc")"
t "  ... the key written before it is put back" "^192.168.0.1$" "$(key LAN_IP_ADDR)"
t "  ... the trial carried on" "phase=trial" "$(cat "$T/status.out")"
t "  ... and expired" "^trial_expired$" "$(st event)"

# ---- status
newboot
out=$(sh "$S" status | tr '\n' ' ')
t "status: the fields" "phase=steady.*applied=0.*uptime=15.*event=.*safe_mode_boot=0" "$out"
sh "$S" try LAN_IP_ADDR=10.1.1.1 > /dev/null
t "status during a trial: the deadline and the new primary" "phase=trial .*trial_deadline=46 .*new_LAN_IP_ADDR=10.1.1.1 " "$(sh "$S" status | tr '\n' ' ')"
sh "$S" revert > /dev/null; sh "$S" settle > /dev/null

# ---- an expiry with a stale id does nothing
newboot
sh "$S" try LAN_IP_ADDR=10.1.1.1 > /dev/null
sh "$S" expire "1.1" > /dev/null
t "an expiry with a stale id does nothing" "^trial$" "$(st phase)"
sh "$S" revert > /dev/null; sh "$S" settle > /dev/null

# ---- a stale lock, its holder dead, is taken over
newboot
mkdir -p "$LOCKDIR"; echo 999999 > "$LOCKDIR/pid"
out=$(sh "$S" try LAN_IP_ADDR=10.1.1.1 2>&1)
t "a stale lock is taken over" "trial_started" "$out"
sh "$S" revert > /dev/null; sh "$S" settle > /dev/null
mkdir -p "$LOCKDIR"
out=$(sh "$S" try LAN_IP_ADDR=10.1.1.1 2>&1)
t "a lock with no pid is stale after the wait" "trial_started" "$out"
sh "$S" revert > /dev/null; sh "$S" settle > /dev/null
mkdir -p "$LOCKDIR"; echo $$ > "$LOCKDIR/pid"
out=$(sh "$S" try LAN_IP_ADDR=10.1.1.1 2>&1)
t "a lock held by a live process makes the caller give up, bounded" "busy" "$out"
rm -rf "$LOCKDIR"

echo
echo "mgmt_addr_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
