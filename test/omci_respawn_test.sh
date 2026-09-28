#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/omci-respawn-reprovision.sh, off the device --
# the automatic re-provisioning svc-omcid.sh backgrounds after init respawns
# a killed omcid (docs/BOOT.md, "Respawn re-provisioning"). Same stub
# pattern as apply_test.sh: /proc/odi_init is a FIFO with a reader that logs
# every verb written to it, so the ORDER is what gets checked -- deactivate,
# then (after the already-running omcid registers) password, activate. This
# script never starts or stops omcid itself, unlike apply.sh's `omci`
# restart -- that is the whole point: init already owns that job.
set -u
cd "$(dirname "$0")/.." || exit 1
R=$PWD/rootfs/skeleton/etc/scripts/omci-respawn-reprovision.sh
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

cat > "$T/bin/pidof" <<PIDOF
#!/bin/sh
[ -f "$T/omcid.pid" ] && p=\$(cat "$T/omcid.pid") && kill -0 "\$p" 2>/dev/null && { echo "\$p"; exit 0; }
exit 1
PIDOF
chmod +x "$T/bin/pidof"
# The stub omcid: a real executable, not an inline subshell function -- this
# sandbox's job control hangs killing a backgrounded `( ... ) &` function by
# a pid read back from a file, the same way apply_test.sh's omcid stub
# avoids it (a real process image behaves like the daemon it stands in for).
cat > "$T/omcid" <<OMCID
#!/bin/sh
echo \$\$ > "$T/omcid.pid"
echo "registered: type=1 pid=\$\$" > "$T/odi_omci"
trap 'rm -f "$T/omcid.pid"; exit 0' TERM
while :; do sleep 1; done
OMCID
chmod +x "$T/omcid"
echo "100.00 90.00" > "$T/uptime"
cat > "$T/cs.xml" <<'XML'
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="GPON_PLOAM_PASSWD" Value="3132333435"/>
	</Dir>
</Config>
XML

run() {
	env PATH="$T/bin:$PATH" RCS_LIB="$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh" \
	    ODI_INIT="$T/odi_init" ODI_OMCI="$T/odi_omci" CS="$T/cs.xml" UPTIME="$T/uptime" \
	    RESUME_DECISION="$T/resume-decision" DECISION_WAIT=0 \
	    DEACT_HOLD=0 REGISTER_WAIT=3 sh "$R" 2>&1
}

# The omcid init already respawned before this script ever runs -- start it
# ourselves to model that, exactly as apply_test.sh's stub registers.
start_omcid() { "$T/omcid" > /dev/null 2>&1 & }

: > "$T/verbs"
start_omcid
sleep 1
run > /dev/null; rc=$?
sleep 1
t "reprovision succeeds" "^0$" "$rc"
verbs=$(tr '\n' ' ' < "$T/verbs")
t "deactivate, password, activate -- in that order, omcid untouched" \
  "^gpondeact gponpw 3132333435 gponact $" "$verbs"
[ -f "$T/omcid.pid" ] && kill "$(cat "$T/omcid.pid")" 2>/dev/null

: > "$T/verbs"
sed -i.bak 's/Value="3132333435"/Value=""/' "$T/cs.xml"
start_omcid
sleep 1
run > /dev/null
sleep 1
t "an empty PLOAM password is not sent, as at boot" "^gpondeact gponact $" "$(tr '\n' ' ' < "$T/verbs")"
[ -f "$T/omcid.pid" ] && kill "$(cat "$T/omcid.pid")" 2>/dev/null

: > "$T/verbs"
env ODI_INIT="$T/no-such-proc-file" RCS_LIB="$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh" \
    RESUME_DECISION="$T/resume-decision" DECISION_WAIT=0 \
    REGISTER_WAIT=1 DEACT_HOLD=0 sh "$R" > /dev/null 2>&1; rc=$?
t "no PON verbs on this kernel (qemu's stock kernel) -- a no-op, not a failure" "^0$" "$rc"
t "and touches nothing" "^0$" "$(wc -c < "$T/verbs" | tr -d ' ')"

# Resume without re-registration (docs/BOOT.md): a "resumed" decision from
# the respawned omcid skips the whole gpondeact/gponact dance.
: > "$T/verbs"
printf 'resumed\n' > "$T/resume-decision"
start_omcid
sleep 1
env PATH="$T/bin:$PATH" RCS_LIB="$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh" \
    ODI_INIT="$T/odi_init" ODI_OMCI="$T/odi_omci" CS="$T/cs.xml" UPTIME="$T/uptime" \
    RESUME_DECISION="$T/resume-decision" DECISION_WAIT=3 \
    DEACT_HOLD=0 REGISTER_WAIT=3 sh "$R" > /dev/null 2>&1; rc=$?
sleep 1
t "a resumed decision succeeds" "^0$" "$rc"
t "and issues no PON verb at all" "^0$" "$(wc -c < "$T/verbs" | tr -d ' ')"
[ -f "$T/omcid.pid" ] && kill "$(cat "$T/omcid.pid")" 2>/dev/null
rm -f "$T/resume-decision"

# No decision file at all within DECISION_WAIT: the safe fallback, not a hang.
: > "$T/verbs"
start_omcid
sleep 1
env PATH="$T/bin:$PATH" RCS_LIB="$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh" \
    ODI_INIT="$T/odi_init" ODI_OMCI="$T/odi_omci" CS="$T/cs.xml" UPTIME="$T/uptime" \
    RESUME_DECISION="$T/no-such-decision-file" DECISION_WAIT=1 \
    DEACT_HOLD=0 REGISTER_WAIT=3 sh "$R" > /dev/null; rc=$?
sleep 1
t "no decision within the wait falls back to reprovision" "^0$" "$rc"
# cs.xml's PLOAM password was cleared by the block above (as at boot), so
# this reprovision sends no gponpw either -- same shape as that test.
t "and still reprovisions" "^gpondeact gponact $" "$(tr '\n' ' ' < "$T/verbs")"
[ -f "$T/omcid.pid" ] && kill "$(cat "$T/omcid.pid")" 2>/dev/null

# svc-omcid.sh: static guard that the marker gate still wires the two
# branches the way docs/BOOT.md describes -- the reprovision script is
# invoked ONLY when the marker already exists (a respawn), and the marker
# is written ONLY on the branch that skips it (the first start).
SVC=$PWD/rootfs/skeleton/etc/scripts/svc-omcid.sh
svc=$(cat "$SVC")
t "svc-omcid.sh checks the marker before touching anything else" \
  'if \[ -e "\$MARKER" \]; then' "$svc"
t "the respawn branch backgrounds the reprovision script" \
  '/etc/scripts/omci-respawn-reprovision\.sh &' "$svc"
t "the first-start branch writes the marker instead" \
  ': > "\$MARKER"' "$svc"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
