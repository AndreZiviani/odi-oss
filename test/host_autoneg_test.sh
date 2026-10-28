#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/host-autoneg.sh, off the device.
#
# The kernel parameter files are plain files here, /proc/uptime is a file
# the sleep stub advances, and the host is a link model the sleep stub
# applies after every second: "forced" has a link only while the stick
# runs auto-negotiation off (param 0), "an" only while it runs it on, "none"
# never, and "late:<uptime>:<model>" behaves as "none" until that uptime.
# The real flash wrapper stores the key, in a temporary odi.conf. setsid
# only records what would be spawned; each test runs the spawned verb
# itself, so the order of events is the test's, not the scheduler's.
#
# What is checked: the key parsing, the boot (default, off, a safe-mode
# boot), the boot watch (forced link, fallback, a host that comes up late,
# no host at all), the trial (expiry, confirm, revert, every refusal),
# status, and the stale lock. The boot counter and the safe mode itself are
# test/safe_mode_test.sh.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/host-autoneg.sh
F=$PWD/rootfs/skeleton/etc/scripts/flash
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
# t NAME PATTERN TEXT: TEXT matches the extended regex PATTERN.
t() { if [[ $3 =~ $2 ]]; then ok "$1"; else no "$1" "wanted /$2/, got: $3"; fi; }
# n NAME PATTERN TEXT: TEXT does not match.
n() { if [[ $3 =~ $2 ]]; then no "$1" "did not want /$2/, got: $3"; else ok "$1"; fi; }

mkdir -p "$T/bin"
# model: the host link as the model says, for the uptime and the mode the
# stick runs now. Run by the sleep stub after every second and by the
# timeout stub before every command, as a real link is always current.
cat > "$T/bin/model" <<EOF
#!/bin/sh
[ -f "$T/model" ] && [ -f "$T/param/host_link" ] || exit 0
up=\$(cut -d. -f1 "$T/uptime")
p=\$(cat "$T/param/host_autoneg" 2>/dev/null)
m=\$(cat "$T/model")
case \$m in late:*)
	at=\${m#late:}; rest=\${at#*:}; at=\${at%%:*}
	if [ "\$up" -ge "\$at" ]; then m=\$rest; else m=none; fi ;;
esac
l=0
case \$m in
forced) [ "\$p" = 0 ] && l=1 ;;
an)     [ "\$p" = 1 ] && l=1 ;;
esac
echo \$l > "$T/param/host_link"
EOF
cat > "$T/bin/timeout" <<EOF
#!/bin/sh
shift
"$T/bin/model"
exec "\$@"
EOF
cat > "$T/bin/setsid" <<EOF
#!/bin/sh
shift 2
echo "\$*" >> "$T/spawned"
EOF
# sleep N: N seconds pass. A hook, when present, runs once inside a sleep
# longer than a second: the trial window, where a confirm or a revert
# would arrive.
cat > "$T/bin/sleep" <<EOF
#!/bin/sh
up=\$(cut -d. -f1 "$T/uptime")
echo "\$((up + \$1)).00 1.00" > "$T/uptime"
"$T/bin/model"
[ -x "$T/hook" ] && [ "\$1" -gt 1 ] && { "$T/hook"; rm -f "$T/hook"; }
exit 0
EOF
chmod +x "$T/bin/"*

export PATH="$T/bin:$PATH"
export FLASH=$F SELF=$S PARAM_DIR=$T/param STATE=$T/run/state LOCKDIR=$T/run/lock
export SAFE_FLAG=$T/run/safe-mode SERDES_STATE=$T/run/host-serdes
export UPTIME=$T/uptime KMSG=$T/kmsg LOG=$T/services.log SLEEP=$T/bin/sleep TIMEOUT=$T/bin/timeout
export BUILD=$T/odi-build
export ODI_CONF=$T/config/odi.conf CS=$T/config/lastgood.xml HS=$T/config/lastgood_hs.xml TMPDIR_=$T
export WATCH_S=5 WATCH_MAX_S=40 TRIAL_S=20
mkdir -p "$T/config" "$T/run" "$T/param"
echo "image=test-image" > "$BUILD"

st() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null; }
key() { sed -n 's/^HOST_AUTONEG=//p' "$ODI_CONF" 2>/dev/null; }
setkey() { "$F" set HOST_AUTONEG "$1" > /dev/null; }

# A fresh boot: /var/run is a tmpfs, the uptime starts again, the kernel
# runs auto-negotiation; the config partition is kept.
newboot() {
	rm -rf "$T/run" "$T/spawned" "$T/hook" "$T/model"
	mkdir -p "$T/run"
	echo "15.00 1.00" > "$T/uptime"
	echo Y > "$T/param/host_autoneg"
	echo "${1:-0}" > "$T/param/host_link"
	: > "$T/kmsg"
}
# spawn runs setsid in the background, so the stub can record it a moment
# after the script returned. A spawn that is expected is polled for (bounded,
# 5 s) as safe_mode_test.sh does; one that is not has nothing to poll for and
# waits a short fixed time on purpose (spawned_none).
spawned() {
	_i=0
	while [ ! -s "$T/spawned" ] && [ "$_i" -lt 100 ]; do /bin/sleep 0.05; _i=$((_i + 1)); done
	cat "$T/spawned" 2>/dev/null
}
spawned_none() { /bin/sleep 0.3; cat "$T/spawned" 2>/dev/null; }

# ---- the key
for v in on off "" OFF yes 0; do
	got=$(sh "$S" parse "$v")
	case $v in off) want=off ;; *) want=on ;; esac
	[ "$got" = "$want" ] && ok "parse '$v' -> $want" || no "parse '$v' -> $want" "got $got"
done

# ---- boot, key absent: nothing written, nothing spawned
newboot 1
out=$(sh "$S" boot)
t "default boot: setting on, running on" "^on on$" "$(st setting) $(st running)"
t "default boot: the kernel parameter untouched" "^Y$" "$(cat "$T/param/host_autoneg")"
t "default boot: nothing spawned" "^$" "$(spawned_none)"
t "default boot: nothing logged to the kernel" "^$" "$(cat "$T/kmsg")"
n "default boot: no output" "." "$out"

# ---- boot, HOST_AUTONEG=off, a forced host
setkey off
newboot 0
echo forced > "$T/model"
sh "$S" boot > /dev/null
t "off boot: the kernel parameter is written 0" "^0$" "$(cat "$T/param/host_autoneg")"
t "off boot: running off, phase watch" "^off watch$" "$(st running) $(st phase)"
t "off boot: the watch spawned" "^watch$" "$(spawned)"
t "off boot: event=applied in the kernel log" "event=applied setting=off running=off" "$(cat "$T/kmsg")"
out=$(sh "$S" try on 2>&1); rc=$?
t "a trial is refused during the boot watch" "boot watch" "$out"
[ "$rc" = 1 ] && ok "  ... with exit 1" || no "  ... with exit 1" "rc $rc"
sh "$S" watch > /dev/null
t "watch, forced host: forced_link, no fallback" "^forced_link 0 off steady$" "$(st event) $(st fallback) $(st running) $(st phase)"
t "watch, forced host: the parameter stays 0" "^0$" "$(cat "$T/param/host_autoneg")"
t "watch, forced host: the key stays off" "^off$" "$(key)"

# ---- the boot fallback: the host negotiates
newboot 0
echo an > "$T/model"
sh "$S" boot > /dev/null
sh "$S" watch > /dev/null
t "watch, negotiating host: fallback, running on" "^fallback 1 on steady$" "$(st event) $(st fallback) $(st running) $(st phase)"
t "watch, negotiating host: the parameter is back to 1" "^1$" "$(cat "$T/param/host_autoneg")"
t "watch, negotiating host: HOST_AUTONEG stays off" "^off$" "$(key)"
t "watch, negotiating host: switched after WATCH_S" "event=watch_switch .*trying auto-negotiation on" "$(cat "$T/kmsg")"
t "status shows the fallback" "fallback=1" "$(sh "$S" status)"
# The fallback clears with the next boot that brings the forced link up.
newboot 0
echo forced > "$T/model"
sh "$S" boot > /dev/null; sh "$S" watch > /dev/null
t "the next boot with a forced host: the fallback is gone" "^forced_link 0$" "$(st event) $(st fallback)"

# ---- a forced host that comes up late: alternate, then off again
newboot 0
echo late:30:forced > "$T/model"
sh "$S" boot > /dev/null; sh "$S" watch > /dev/null
t "late forced host: back to off, link, no fallback" "^forced_link 0 off$" "$(st event) $(st fallback) $(st running)"
t "late forced host: it went on, then off again" "trying auto-negotiation on.*trying auto-negotiation off" "$(tr '\n' ' ' < "$T/kmsg")"

# ---- no host at all: bounded, ends on auto-negotiation
newboot 0
echo none > "$T/model"
sh "$S" boot > /dev/null; sh "$S" watch > /dev/null
t "no host: fallback_nolink, running on" "^fallback_nolink 1 on steady$" "$(st event) $(st fallback) $(st running) $(st phase)"
up=$(cut -d. -f1 "$T/uptime")
[ "$up" -le $((15 + 40 + 5 + 1)) ] && ok "no host: the watch ended within WATCH_MAX_S (uptime $up)" || no "no host: bounded" "uptime $up"


# ---- a value that is neither: on, logged
setkey sideways 2>/dev/null
newboot 1
sh "$S" boot > /dev/null
t "HOST_AUTONEG=sideways runs on" "^on on$" "$(st setting) $(st running)"
t "  ... and says so" "event=bad_value" "$(cat "$T/kmsg")"
setkey ""

# ---- a safe-mode boot (safe-mode.sh set the flag): nothing applied, no trial
setkey off
newboot 1
echo "short_boots=3" > "$SAFE_FLAG"
sh "$S" boot > /dev/null
t "safe-mode boot: auto-negotiation, nothing written" "^on Y steady safe_mode_boot$" "$(st running) $(cat "$T/param/host_autoneg") $(st phase) $(st event)"
t "safe-mode boot: no watch" "^$" "$(spawned_none)"
out=$(sh "$S" try off 2>&1)
t "safe-mode boot: a trial is refused" "safe-mode boot" "$out"
t "status says so" "safe_mode_boot=1" "$(sh "$S" status)"
rm -f "$SAFE_FLAG"
setkey ""

# ---- the trial
newboot 1
echo forced > "$T/model"
sh "$S" boot > /dev/null
out=$(sh "$S" try off)
t "try off: started" "event=trial_started .*trying auto-negotiation off for 20 s" "$out"
t "try off: nothing written yet (the answer leaves first)" "^Y$" "$(cat "$T/param/host_autoneg")"
t "try off: phase trial, deadline uptime+21" "^trial 36$" "$(st phase) $(st trial_deadline)"
id=$(st trial_id)
t "try off: trial-run spawned with its id" "^trial-run $id 20$" "$(spawned)"
out=$(sh "$S" confirm 2>&1)
t "confirm before the write is refused" "not applied yet" "$out"
out=$(sh "$S" try on 2>&1)
t "a second trial is refused" "already running" "$out"
sh "$S" trial-run "$id" 20 > /dev/null
t "expiry: back to on" "^trial_expired on steady$" "$(st event) $(st running) $(st phase)"
t "expiry: the parameter is 1 again" "^1$" "$(cat "$T/param/host_autoneg")"
t "expiry: the key is not stored" "^$" "$(key)"
t "expiry: logged" "event=trial_expired .*not confirmed in time" "$(cat "$T/kmsg")"

# confirm inside the window: stored, and the expiry that follows is a no-op
: > "$T/spawned"
sh "$S" try off > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" confirm >> "%s/confirm.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null
t "confirm: stored HOST_AUTONEG=off" "^off$" "$(key)"
t "confirm: state" "^trial_confirmed off off steady$" "$(st event) $(st setting) $(st running) $(st phase)"
t "confirm: the expiry after it changed nothing" "^0$" "$(cat "$T/param/host_autoneg")"
out=$(sh "$S" confirm 2>&1); rc=$?
t "confirm with no trial is refused" "no trial is running" "$out"
[ "$rc" = 1 ] && ok "  ... with exit 1" || no "  ... with exit 1" "rc $rc"

# revert inside the window, from off back to off; then trying on
: > "$T/spawned"
sh "$S" try on > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" revert >> "%s/revert.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null
t "revert: back to off, the key unchanged" "^trial_reverted off off$" "$(st event) $(st running) $(key)"
t "revert: the parameter is 0 again" "^0$" "$(cat "$T/param/host_autoneg")"

# confirming on removes the key
sh "$S" try on > /dev/null
id=$(st trial_id)
printf '#!/bin/sh\nsh "%s" confirm >> "%s/confirm.out" 2>&1\n' "$S" "$T" > "$T/hook"; chmod +x "$T/hook"
sh "$S" trial-run "$id" 20 > /dev/null
t "confirm on: the key is removed" "^$" "$(key)"
t "confirm on: setting on" "^trial_confirmed on on$" "$(st event) $(st setting) $(st running)"

# the refusals
out=$(sh "$S" try on 2>&1)
t "try on while on: nothing to try" "already on" "$out"
out=$(sh "$S" try sideways 2>&1)
t "try with a bad mode is refused" "usage" "$out"
out=$(sh "$S" try off 5 2>&1)
t "try with too short a window is refused" "10 to" "$out"
out=$(sh "$S" try off 9999 2>&1)
t "try with too long a window is refused" "10 to" "$out"
sh "$S" try off > /dev/null
sh "$S" expire "1.1" > /dev/null
t "an expiry with a stale id does nothing" "^trial$" "$(st phase)"
sh "$S" revert > /dev/null

# ---- status
out=$(sh "$S" status)
t "status: the fields" "setting=on.*running=on.*phase=steady.*uptime=[0-9]+.*link=[01].*safe_mode_boot=0" "$(printf '%s' "$out" | tr '\n' ' ')"
rm -f "$T/param/host_link"
t "status: link unknown without the parameter" "link=unknown" "$(sh "$S" status)"
echo 1 > "$T/param/host_link"

# ---- a stale lock, its holder dead, is taken over
mkdir -p "$LOCKDIR"; echo 999999 > "$LOCKDIR/pid"
out=$(sh "$S" try off 2>&1)
t "a stale lock is taken over" "trial_started" "$out"
sh "$S" revert > /dev/null
mkdir -p "$LOCKDIR"
out=$(sh "$S" try off 2>&1)
t "a lock with no pid is stale after the wait" "trial_started" "$out"
sh "$S" revert > /dev/null
mkdir -p "$LOCKDIR"; echo $$ > "$LOCKDIR/pid"
out=$(sh "$S" try off 2>&1)
t "a lock held by a live process makes the caller give up, bounded" "busy" "$out"
rm -rf "$LOCKDIR"

# ---- no kernel support: off is not applied
rm -f "$T/param/host_autoneg"
setkey off
newboot 0 2>/dev/null; rm -f "$T/param/host_autoneg"
sh "$S" boot > /dev/null
t "no kernel parameter: running on, logged" "^no_kernel on$" "$(st event) $(st running)"

echo
echo "host_autoneg_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
