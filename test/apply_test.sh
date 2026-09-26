#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/apply.sh, off the device.
#
# /proc/odi_init is a FIFO here with a reader that logs every verb written to
# it, so the ORDER is what gets checked: deactivate, stop the old omcid, start
# a new one with -r, password, activate. omcid is a stub that registers in a
# fake /proc/odi_omci and waits for SIGTERM, as the real one deregisters on it.
set -u
cd "$(dirname "$0")/.." || exit 1
A=$PWD/rootfs/skeleton/etc/scripts/apply.sh
T=$(mktemp -d)
pass=0; fail=0
t() {
	if printf '%s' "$3" | grep -q -- "$2"; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        wanted /$2/, got:"; printf '%s\n' "$3" | sed 's/^/        /'; fail=$((fail + 1)); fi
}
mkdir -p "$T/bin"
mkfifo "$T/odi_init"
( while :; do cat "$T/odi_init" >> "$T/verbs" 2>/dev/null || break; done ) &
reader=$!
cleanup() { kill "$reader" 2>/dev/null; [ -f "$T/omcid.pid" ] && kill "$(cat "$T/omcid.pid")" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

printf '#!/bin/sh\nexec "$@"\n' > "$T/bin/setsid"
cat > "$T/bin/pidof" <<PIDOF
#!/bin/sh
[ -f "$T/omcid.pid" ] && p=\$(cat "$T/omcid.pid") && kill -0 "\$p" 2>/dev/null && { echo "\$p"; exit 0; }
exit 1
PIDOF
cat > "$T/omcid" <<OMCID
#!/bin/sh
echo "omcid \$*" >> "$T/verbs"
echo \$\$ > "$T/omcid.pid"
echo "registered: type=1 pid=\$\$" > "$T/odi_omci"
trap 'rm -f "$T/omcid.pid"; exit 0' TERM
while :; do sleep 1; done
OMCID
chmod +x "$T/bin/setsid" "$T/bin/pidof" "$T/omcid"
echo "100.00 90.00" > "$T/uptime"
cat > "$T/cs.xml" <<'XML'
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="GPON_PLOAM_PASSWD" Value="3132333435"/>
	</Dir>
</Config>
XML

run() {
	env PATH="$T/bin:$PATH" ODI_INIT="$T/odi_init" ODI_OMCI="$T/odi_omci" OMCID="$T/omcid" \
	    OMCID_LOG="$T/omcid.log" CS="$T/cs.xml" MODULES_OFF="$T/modules.off" UPTIME="$T/uptime" \
	    DEACT_HOLD=0 NETWORK="$T/network.sh" SERVICES_LOG="$T/services.log" sh "$A" "$@" 2>&1
}

# A running omcid to replace.
"$T/omcid" > /dev/null 2>&1 &
sleep 1
first=$(cat "$T/omcid.pid")
: > "$T/verbs"

out=$(run omci); rc=$?
sleep 1
t "apply omci succeeds" "^0$" "$rc"
t "it says the internet is down while it runs" "internet is down" "$out"
t "the old omcid is stopped" "omcid $first stopped" "$out"
t "a new one registers" "registered for OMCI" "$out"
verbs=$(tr '\n' ' ' < "$T/verbs")
t "deactivate, restart with -r, password, activate -- in that order" \
  "^gpondeact omcid -a -d -r gponpw 3132333435 gponact $" "$verbs"
t "and the new omcid is not the old one" "yes" "$([ "$(cat "$T/omcid.pid")" != "$first" ] && echo yes)"

: > "$T/verbs"
sed -i.bak 's/Value="3132333435"/Value=""/' "$T/cs.xml"
run omci > /dev/null
sleep 1
t "an empty PLOAM password is not sent, as at boot" "^gpondeact omcid -a -d -r gponact $" "$(tr '\n' ' ' < "$T/verbs")"

touch "$T/modules.off"; : > "$T/verbs"
out=$(run omci); rc=$?
t "modules.off refuses" "no omcid" "$out"
t "and touches nothing" "^0$" "$(wc -c < "$T/verbs" | tr -d ' ')"
rm -f "$T/modules.off"

cat > "$T/network.sh" <<'NET'
#!/bin/sh
echo "network.sh $*" >> "$STUBLOG"
NET
chmod +x "$T/network.sh"
export STUBLOG="$T/netcalls"
out=$(run network); rc=$?
sleep 2
t "apply network prints the plan first" "^network.sh addr -n" "$(sed -n 1p "$T/netcalls")"
t "then applies it, detached" "^network.sh addr$" "$(sed -n 2p "$T/netcalls")"
t "and returns at once" "applying in one second" "$out"

t "an unknown verb is refused" "usage" "$(run bogus)"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
