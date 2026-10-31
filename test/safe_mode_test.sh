#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/safe-mode.sh, off the device, over simulated
# boots: the counter on a temporary config partition, the uptime a file,
# mount a stub that records the bind mount, the real flash wrapper on a
# temporary odi.conf. Also the two PON steps of rcs-lib.sh that a safe-mode
# boot skips.
#
# What is checked: the count and its log line (with the uptime); five short
# boots before the safe one by default, three with HOST_AUTONEG=off; a boot
# that reaches 60 s resets the count; an unreadable counter counts as 0; no
# writable config, no count; the safe mode: the flag of this boot, the
# record, HOST_AUTONEG cleared only when it was off, nothing else written to
# the config, the root password bind-mounted from RAM, the counter gone;
# the next boot is normal and keeps the record until ack; status and active;
# HOST_SERDES other than fiber1g counts like HOST_AUTONEG=off and is
# removed by the safe boot, alone or with HOST_AUTONEG;
# the laser and gponact PON steps skipped on a safe-mode boot only.
# The watchdog trigger: the previous boot's reason (a stand-in for
# /proc/odi_ramlog_prev) counts wdt_*, panic and oops, power leaves the count,
# anything else clears it; three in a row make the safe boot, one or two
# never do, a boot that lives 600 s clears it, a safe boot clears it and is
# not followed by another. A config partition that is not usable makes the
# boot a safe one with no record. confirm_watchdog confirms a safe boot
# without the mount and address checks, and a normal boot with them.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/safe-mode.sh
F=$PWD/rootfs/skeleton/etc/scripts/flash
LIB=$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
t() { if [[ $3 =~ $2 ]]; then ok "$1"; else no "$1" "wanted /$2/, got: $3"; fi; }
n() { if [[ $3 =~ $2 ]]; then no "$1" "did not want /$2/, got: $3"; else ok "$1"; fi; }

mkdir -p "$T/bin" "$T/config" "$T/run"
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
cat > "$T/bin/sleep" <<EOF
#!/bin/sh
up=\$(cut -d. -f1 "$T/uptime")
echo "\$((up + \$1)).00 1.00" > "$T/uptime"
EOF
cat > "$T/bin/mount" <<EOF
#!/bin/sh
echo "mount \$*" >> "$T/mounts"
EOF
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH"
export FLASH=$F SELF=$S COUNTER=$T/config/boot-count RECORD=$T/config/safe-mode FLAG=$T/run/safe-mode
export PASSWD=$T/etc-passwd PASSWD_RUN=$T/run/safe-mode.passwd CONFIG_DIR=$T/config
export PREV=$T/prev WDTFILE=$T/config/boot-wdt
export UPTIME=$T/uptime KMSG=$T/kmsg BUILD=$T/odi-build LOG=$T/services.log
export SLEEP=$T/bin/sleep TIMEOUT=$T/bin/timeout MOUNT=$T/bin/mount
export ODI_CONF=$T/config/odi.conf CS=$T/config/lastgood.xml HS=$T/config/lastgood_hs.xml TMPDIR_=$T
echo "image=test-image" > "$BUILD"
echo 'root:!:0:0:root:/root:/bin/sh' > "$PASSWD"

key() { sed -n 's/^HOST_AUTONEG=//p' "$ODI_CONF" 2>/dev/null; }
setkey() { "$F" set HOST_AUTONEG "$1" > /dev/null; }
newboot() {
	rm -rf "$T/run" "$T/spawned" "$T/mounts"
	mkdir -p "$T/run"
	echo "${1:-12}.00 1.00" > "$T/uptime"
	: > "$T/kmsg"
}
# The stub setsid records the spawn from a background job, so it can land a
# moment after safe-mode.sh returned: poll for it (bounded) instead of a fixed
# sleep, which lost the race on a loaded host. spawned_none is the other
# case, where nothing is expected: there is nothing to poll for, so it waits a
# short fixed time on purpose.
spawned() {
	i=0
	while [ ! -s "$T/spawned" ] && [ "$i" -lt 100 ]; do /bin/sleep 0.05; i=$((i + 1)); done
	cat "$T/spawned" 2>/dev/null
}
spawned_none() { /bin/sleep 0.3; cat "$T/spawned" 2>/dev/null; }
count() { cat "$COUNTER" 2>/dev/null; }
config_files() { (cd "$T/config" && ls -A | sort | tr '\n' ' '); }

# ---- counting, default limit 5
setkey ""
for i in 1 2 3 4 5; do
	newboot 12
	sh "$S" count > /dev/null
	t "boot $i: counted $i" "^$i$" "$(count)"
done
t "the count logs the boot, the limit and the uptime" "event=boot_count boot=5 short_before=4 limit=5 wdt_before=0 wdt_limit=3 prev=none uptime=12" "$(cat "$T/kmsg")"
t "a counted boot spawns boot-ok" "boot-ok" "$(spawned)"
[ ! -f "$FLAG" ] && ok "four short boots before: not yet" || no "four short boots before: not yet"
newboot 14
sh "$S" count > /dev/null
t "five short boots before: safe mode" "event=safe_mode cause=short_boots short_boots=5 wdt_resets=0 cleared=none root_password=default" "$(cat "$T/kmsg")"
t "  the flag of this boot" "cause=short_boots.*short_boots=5.*cleared=none.*build=test-image.*root_password=default" "$(tr '\n' ' ' < "$FLAG")"
t "  the record on the config partition" "short_boots=5.*cleared=none" "$(tr '\n' ' ' < "$RECORD")"
[ ! -f "$COUNTER" ] && ok "  the counter is gone" || no "  the counter is gone"
t "  the only config change is the record (and the counter removed)" "^safe-mode $" "$(config_files | sed 's/odi.conf //')"
t "  root: the RAM copy is bind-mounted over the account file" "^mount -o bind $PASSWD_RUN $PASSWD$" "$(cat "$T/mounts")"
t "  root: the RAM copy carries the default password hash" '^root:\$6\$odisafemode\$[^:]+:0:0:root:/root:/bin/sh$' "$(cat "$PASSWD_RUN")"
t "  the account file on flash is untouched" '^root:!:0:0' "$(cat "$PASSWD")"
t "  nothing spawned" "^$" "$(spawned_none)"
sh "$S" active && ok "  active says so" || no "  active says so"
t "  status: this boot and the record" "active=1.*now_short_boots=5.*record=1.*record_short_boots=5" "$(sh "$S" status | tr '\n' ' ')"

# ---- the next boot is normal, the record stays until ack
newboot 70
sh "$S" count > /dev/null
sh "$S" active && no "the next boot is not a safe one" || ok "the next boot is not a safe one"
t "  counted from 1 again" "^1$" "$(count)"
t "  the record is still there" "record=1" "$(sh "$S" status)"
sh "$S" ack > /dev/null
[ ! -f "$RECORD" ] && ok "ack removes the record" || no "ack removes the record"
t "  and logs it" "event=safe_mode_ack" "$(cat "$T/kmsg")"
t "status after ack" "active=0.*record=0" "$(sh "$S" status | tr '\n' ' ')"
sh "$S" boot-ok > /dev/null
[ ! -f "$COUNTER" ] && ok "boot-ok clears the counter at 60 s" || no "boot-ok clears the counter at 60 s"

# ---- HOST_AUTONEG=off: three short boots are enough, and the key goes
setkey off
for i in 1 2 3; do newboot 12; sh "$S" count > /dev/null; done
[ ! -f "$FLAG" ] && ok "key off: two short boots before is not enough" || no "key off: two short boots before is not enough"
t "key off: the limit is 3" "limit=3" "$(cat "$T/kmsg")"
newboot 12
sh "$S" count > /dev/null
t "key off: the fourth boot is the safe one" "event=safe_mode cause=short_boots short_boots=3 wdt_resets=0 cleared=HOST_AUTONEG" "$(cat "$T/kmsg")"
t "  HOST_AUTONEG is cleared" "^$" "$(key)"
sh "$S" ack > /dev/null

# ---- HOST_SERDES other than fiber1g: three short boots too, and the key
# goes, as HOST_AUTONEG=off does: Fiber 1G is the fallback, the most
# compatible host mode.
"$F" set HOST_SERDES 2500basex > /dev/null
for i in 1 2 3; do newboot 12; sh "$S" count > /dev/null; done
[ ! -f "$FLAG" ] && ok "HOST_SERDES=2500basex: two short boots before is not enough" || no "HOST_SERDES=2500basex: two short boots before is not enough"
t "HOST_SERDES=2500basex: the limit is 3" "limit=3" "$(cat "$T/kmsg")"
newboot 12
sh "$S" count > /dev/null
t "HOST_SERDES=2500basex: the fourth boot is the safe one, the key cleared" "event=safe_mode cause=short_boots short_boots=3 wdt_resets=0 cleared=HOST_SERDES root_password=default" "$(cat "$T/kmsg")"
t "  HOST_SERDES is removed" "^$" "$(sed -n 's/^HOST_SERDES=//p' "$ODI_CONF")"
t "  the flag and the record say so" "cleared=HOST_SERDES.*cleared=HOST_SERDES" "$(tr '\n' ' ' < "$FLAG") $(tr '\n' ' ' < "$RECORD")"
sh "$S" ack > /dev/null
"$F" set HOST_SERDES hisgmii > /dev/null
setkey off
for i in 1 2 3 4; do newboot 12; sh "$S" count > /dev/null; done
t "HOST_SERDES=hisgmii and HOST_AUTONEG=off: both cleared, in that order" "cleared=HOST_AUTONEG,HOST_SERDES " "$(cat "$T/kmsg")"
t "  both keys gone" "^ $" "$(key) $(sed -n 's/^HOST_SERDES=//p' "$ODI_CONF")"
sh "$S" ack > /dev/null; rm -f "$FLAG"
"$F" set HOST_SERDES sideways > /dev/null
newboot 12; sh "$S" count > /dev/null
t "HOST_SERDES with a bad value (it runs fiber1g): the limit stays 5" "limit=5" "$(cat "$T/kmsg")"
rm -f "$COUNTER"
"$F" set HOST_SERDES hisgmii > /dev/null
rm -f "$WDTFILE"
for i in 1 2 3; do
	newboot 12; echo "previous boot: boot=8 slot=1 build=x crumb=y reason=wdt_userland" > "$PREV"
	sh "$S" count > /dev/null; [ -f "$FLAG" ] || rm -f "$COUNTER"
done
t "a watchdog safe mode leaves HOST_SERDES alone, as it does HOST_AUTONEG" "cause=watchdog.*cleared=none" "$(tr '\n' ' ' < "$FLAG")"
t "  HOST_SERDES still set" "^hisgmii$" "$(sed -n 's/^HOST_SERDES=//p' "$ODI_CONF")"
sh "$S" ack > /dev/null; rm -f "$FLAG" "$PREV" "$WDTFILE" "$COUNTER"
"$F" set HOST_SERDES "" > /dev/null

# ---- a long boot in between starts again; an unreadable counter is 0
setkey ""
newboot 12; sh "$S" count > /dev/null
newboot 12; sh "$S" count > /dev/null; sh "$S" boot-ok > /dev/null
newboot 12; sh "$S" count > /dev/null
t "a boot that reached 60 s resets the count" "^1$" "$(count)"
echo garbage > "$COUNTER"
newboot 12; sh "$S" count > /dev/null
t "an unreadable counter counts as 0" "^1$" "$(count)"
rm -f "$COUNTER"

# ---- no usable config partition: the safe mode at once, with no record
# Root writes anywhere, so a read-only directory is no obstacle to it (this
# runs as root in a container, on htpc and in CI). A config directory that is
# not a directory at all fails for every user; the read-only one is checked
# too when the test is not root. rcS calls no-config itself when the
# partition is not mounted.
newboot 12
: > "$T/not-a-dir"
out=$(CONFIG_DIR=$T/not-a-dir COUNTER=$T/not-a-dir/boot-count sh "$S" count 2>&1)
t "no writable config (not a directory): safe mode" "event=safe_mode cause=config short_boots=0 wdt_resets=0" "$out"
t "  the flag of this boot says why" "^cause=config$" "$(grep '^cause' "$FLAG")"
t "  root: the default password is in place" "root_password=default" "$(cat "$FLAG")"
[ ! -e "$RECORD" ] && ok "  no record: there is nowhere to write it" || no "  no record: there is nowhere to write it"
if [ "$(id -u)" != 0 ]; then
	chmod 500 "$T/config"
	newboot 12
	out=$(sh "$S" count 2>&1)
	t "no writable config (read-only): safe mode" "cause=config" "$out"
	chmod 700 "$T/config"
else
	echo "skip  no writable config (read-only): root can write to it"
fi
newboot 12
out=$(sh "$S" no-config 2>&1)
t "no-config (rcS: not mounted): safe mode" "event=safe_mode cause=config" "$out"
sh "$S" active && ok "  active says so" || no "  active says so"
t "  the PON steps skip, as for any safe boot" "cause=config" "$(sh "$S" status | tr '\n' ' ')"
newboot 12
sh "$S" count > /dev/null
sh "$S" active && no "  the next boot, with the partition back, is normal" || ok "  the next boot, with the partition back, is normal"
t "  and counted" "^1$" "$(count)"
rm -f "$COUNTER" "$RECORD"

# ---- the watchdog trigger. prev REASON stands in for /proc/odi_ramlog_prev.
prev() {
	if [ -z "$1" ]; then rm -f "$PREV"; return; fi
	printf 'this boot: boot=9 slot=1\nprevious boot: boot=8 slot=1 build=x crumb=TICK/15028 reason=%s\n---- page A: first 10 bytes ----\nprevious boot: reason=reboot\n' "$1" > "$PREV"
}
wdtn() { cat "$WDTFILE" 2>/dev/null; }
boot_after() { newboot "${2:-12}"; prev "$1"; sh "$S" count > /dev/null; }
rm -f "$COUNTER" "$WDTFILE" "$RECORD" "$FLAG"

# three boots in a row that ended in a watchdog reset, the unconfirmed-userland
# loop: each boot lives 123 s, so it clears the short-boot counter (the rm
# below stands for boot-ok at 60 s) but never reaches the 600 s that clear
# the watchdog counter
boot_after wdt_userland
t "after one wdt_userland: counted 1" "^1$" "$(wdtn)"
[ ! -f "$FLAG" ] && ok "  one watchdog reset is not a safe mode" || no "  one watchdog reset is not a safe mode"
t "  the boot log names the reason and the count" "event=boot_count .*wdt_before=1 wdt_limit=3 prev=wdt_userland" "$(cat "$T/kmsg")"
rm -f "$COUNTER"
boot_after wdt_client:omcid
t "a client miss counts too: 2" "^2$" "$(wdtn)"
[ ! -f "$FLAG" ] && ok "  two in a row are not a safe mode" || no "  two in a row are not a safe mode"
rm -f "$COUNTER"
boot_after wdt_userland
t "the third wdt reset: safe mode" "event=safe_mode cause=watchdog short_boots=0 wdt_resets=3 cleared=none root_password=default" "$(cat "$T/kmsg")"
t "  the flag and the record say watchdog" "cause=watchdog.*wdt_resets=3" "$(tr '\n' ' ' < "$RECORD")"
[ ! -f "$WDTFILE" ] && [ ! -f "$COUNTER" ] && ok "  both counters are gone" || no "  both counters are gone"
t "  nothing spawned" "^$" "$(spawned_none)"
sh "$S" ack > /dev/null

# the safe boot is followed by a normal one, not another safe one
boot_after reboot 70
[ ! -f "$FLAG" ] && ok "the boot after the safe one is normal" || no "the boot after the safe one is normal"
t "  with nothing counted" "^$" "$(wdtn)"
sh "$S" boot-ok > /dev/null

# a safe boot that itself ends in a watchdog reset starts the count again: it
# takes three more to come back, the loop is bounded and each safe boot stays up
boot_after wdt_userland
t "a reset after the safe boot counts from 1" "^1$" "$(wdtn)"
rm -f "$COUNTER" "$WDTFILE"

# the other reasons
boot_after panic;  t "panic counts" "^1$" "$(wdtn)"; rm -f "$COUNTER"
boot_after oops;   t "oops counts" "^2$" "$(wdtn)"; rm -f "$COUNTER"
boot_after power;  t "power (a power cycle) leaves the count" "^2$" "$(wdtn)"; rm -f "$COUNTER"
boot_after "";     t "no ramlog (an older kernel) leaves the count" "^2$" "$(wdtn)"; rm -f "$COUNTER"
boot_after unknown; t "unknown clears it" "^$" "$(wdtn)"; rm -f "$COUNTER"
boot_after wdt_mem; boot_after wdt_cpu_rx
t "wdt_mem and wdt_cpu_rx count" "^2$" "$(wdtn)"; rm -f "$COUNTER"
boot_after reboot;  t "a reboot clears it" "^$" "$(wdtn)"; rm -f "$COUNTER"
boot_after wdt_userland; boot_after halt
t "halt clears it" "^$" "$(wdtn)"; rm -f "$COUNTER"
boot_after wdt_userland; boot_after poweroff
t "poweroff clears it" "^$" "$(wdtn)"; rm -f "$COUNTER"
t "the first line of the file (this boot) is never read as a reason" "^$" "$(wdtn)"

# a boot that lives WDT_OK_S clears the watchdog count, 60 s the short one
boot_after wdt_userland
sh "$S" boot-ok > /dev/null
[ ! -f "$COUNTER" ] && [ ! -f "$WDTFILE" ] && ok "boot-ok waits for 600 s and clears both counters" || no "boot-ok waits for 600 s and clears both counters"
t "  at 600 s of uptime" "^600" "$(cat "$T/uptime")"
rm -f "$COUNTER"
newboot 12; prev reboot; sh "$S" count > /dev/null
sh "$S" boot-ok > /dev/null
t "boot-ok with no watchdog count stops at 60 s" "^60" "$(cat "$T/uptime")"

# a rare reset hours apart never adds up
for i in 1 2 3 4; do boot_after wdt_client:omcid; sh "$S" boot-ok > /dev/null; done
[ ! -f "$FLAG" ] && ok "resets each followed by a boot that lives 10 minutes never make a safe mode" || no "resets each followed by a boot that lives 10 minutes never make a safe mode"

# the short-boot rule is unchanged next to it, and short boots win the record
prev ""; rm -f "$COUNTER" "$WDTFILE"
for i in 1 2 3 4 5; do newboot 12; sh "$S" count > /dev/null; done
newboot 14; prev wdt_userland; sh "$S" count > /dev/null
t "five short boots and a watchdog reset: the short-boot cause is named" "event=safe_mode cause=short_boots short_boots=5 wdt_resets=1" "$(cat "$T/kmsg")"
sh "$S" ack > /dev/null
rm -f "$PREV" "$WDTFILE"

# status shows the watchdog count
newboot 12; prev wdt_userland; sh "$S" count > /dev/null
t "status: the watchdog count" "boot_count=1.*wdt_count=1" "$(sh "$S" status | tr '\n' ' ')"
rm -f "$COUNTER" "$WDTFILE" "$PREV"

# ---- a failed bind mount is said, not hidden
for i in 1 2 3 4 5 6; do newboot 12; MOUNT=false sh "$S" count > /dev/null; done
t "a failed root password override is recorded" "root_password=failed" "$(cat "$FLAG")"
sh "$S" ack > /dev/null; rm -f "$FLAG"

# ---- rcs-lib.sh: the PON steps a safe-mode boot skips
pon() {
	(
		# shellcheck source=/dev/null
		. "$LIB"
		# shellcheck disable=SC2034 # read by the sourced functions
		SAFE_FLAG=$1
		crumb() { echo "crumb $*"; }
		pon_step() { echo "step $1"; }
		pon_step_laser
		pon_step_gponact
	) 2>&1
}
echo x > "$T/run/flag"
out=$(pon "$T/run/flag")
t "safe-mode boot: the laser step leaves the laser off" "crumb pon step laser: safe-mode boot, laser left off" "$out"
t "safe-mode boot: gponact is skipped" "crumb pon step gponact: safe-mode boot, not activated" "$out"
printf '%s\n' "$out" | grep -q '^step ' && no "safe-mode boot: no laser or gponact verb written" "$out" || ok "safe-mode boot: no laser or gponact verb written"
rm -f "$T/run/flag"
out=$(pon "$T/run/flag")
t "a normal boot writes the laser verb" "(^|.)step laser" "$(printf '%s\n' "$out" | grep '^step ')"
t "  and gponact" "step gponact" "$(printf '%s\n' "$out" | grep '^step ')"

# ---- confirm_watchdog: a safe boot confirms without the mount and address
# checks (a dead config partition is why it is one), a normal boot does not.
confirm() {
	(
		# shellcheck source=/dev/null
		. "$LIB"
		# shellcheck disable=SC2034 # read by the sourced functions
		SAFE_FLAG=$1 WDOK=$T/wdok SWITCH_INIT_OK=${2:-1}
		crumb() { echo "crumb $*"; }
		required_mounts_ok() { return 1; }
		config_mounted() { return 1; }
		dev_confirm_ok() { return 1; }
		: > "$WDOK"
		confirm_watchdog
		echo "wdok=$(cat "$WDOK")"
	) 2>&1
}
echo x > "$T/run/flag"
out=$(confirm "$T/run/flag")
t "safe boot: confirmed with no mounts, no config, no address" "safe-mode boot: userland up.*wdok=1" "$(printf '%s ' $out)"
out=$(confirm "$T/run/flag" 0)
t "safe boot, switch init failed: not confirmed" "switch_init failed" "$out"
t "  and nothing written" "wdok=$" "$out"
rm -f "$T/run/flag"
out=$(confirm "$T/run/flag")
t "normal boot, mounts missing: not confirmed" "NOT confirmed: a required mount is missing.*wdok=( |$)" "$(printf '%s ' $out)"

echo
echo "safe_mode_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
