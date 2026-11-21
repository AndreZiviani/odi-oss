#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

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
# The login: the default credentials only after a power cycle, per cause and
# per reason of the previous boot (login= in the flag, the bind mount made
# or not), and what svc-dropbear.sh makes of it on a keys-only image. The
# timed exit: a safe boot spawns exit, which reboots (a stub) at 1800 s of
# uptime unless ack held it, for every cause, the one with no record too;
# an ack after the exit claimed the flag fails and keeps the record; the
# exit waits for a firmware write, bounded and measured in uptime; a sleep
# that fails is no deadline; the exit lock held by another process (a real
# flock, the real timeout). The flag is whole at its one rename, the record
# follows it, and a flag that cannot be written is said, with what became
# of the bind mount.
set -u
cd "$(dirname "$0")/.." || exit 1
# shellcheck source=test/host-flock.sh
. test/host-flock.sh
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
cat > "$T/bin/reboot" <<EOF
#!/bin/sh
echo "reboot \$*" >> "$T/reboots"
EOF
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH"
export FLASH=$F SELF=$S COUNTER=$T/config/boot-count RECORD=$T/config/safe-mode FLAG=$T/run/safe-mode
export PASSWD=$T/etc-passwd PASSWD_RUN=$T/run/safe-mode.passwd CONFIG_DIR=$T/config
export PREV=$T/prev WDTFILE=$T/config/boot-wdt
export UPTIME=$T/uptime KMSG=$T/kmsg BUILD=$T/odi-build LOG=$T/services.log
export SLEEP=$T/bin/sleep TIMEOUT=$T/bin/timeout MOUNT=$T/bin/mount REBOOT=$T/bin/reboot
export ODI_CONF=$T/config/odi.conf CS=$T/config/lastgood.xml HS=$T/config/lastgood_hs.xml TMPDIR_=$T
echo "image=test-image" > "$BUILD"
echo 'root:!:0:0:root:/root:/bin/sh' > "$PASSWD"

key() { sed -n 's/^HOST_AUTONEG=//p' "$ODI_CONF" 2>/dev/null; }
setkey() { "$F" set HOST_AUTONEG "$1" > /dev/null; }
newboot() {
	rm -rf "$T/run" "$T/spawned" "$T/mounts" "$T/reboots"
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
t "  it spawns the timed exit, and no boot-ok" "^exit$" "$(spawned)"
t "  no ramlog and short boots: the default login, and the flag says when it ends" "prev=none login=default root_password=default exit_at=1800 exit=timed" "$(tr '\n' ' ' < "$FLAG")"
t "  the record names the login, not the exit" "login=default $" "$(tr '\n' ' ' < "$RECORD")"
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

# ---- OMCI_UNI_LOCK=1: an OLT lock cuts the host link, so the way back is
# five short boots, and that safe mode removes the key. The limit stays 5:
# a locked stick boots normally. A value other than 1 is the default and is
# left alone.
"$F" set OMCI_UNI_LOCK 1 > /dev/null
for i in 1 2 3 4 5; do newboot 12; sh "$S" count > /dev/null; done
[ ! -f "$FLAG" ] && ok "OMCI_UNI_LOCK=1: the limit stays 5" || no "OMCI_UNI_LOCK=1: the limit stays 5"
newboot 12; sh "$S" count > /dev/null
t "OMCI_UNI_LOCK=1: the sixth boot is the safe one, the key cleared" "event=safe_mode cause=short_boots short_boots=5 wdt_resets=0 cleared=OMCI_UNI_LOCK root_password=default" "$(cat "$T/kmsg")"
t "  OMCI_UNI_LOCK is removed" "^$" "$(sed -n 's/^OMCI_UNI_LOCK=//p' "$ODI_CONF")"
sh "$S" ack > /dev/null; rm -f "$FLAG"
"$F" set OMCI_UNI_LOCK 0 > /dev/null
for i in 1 2 3 4 5 6; do newboot 12; sh "$S" count > /dev/null; done
t "OMCI_UNI_LOCK=0 is not cleared" "cleared=none" "$(cat "$T/kmsg")"
t "  still set" "^0$" "$(sed -n 's/^OMCI_UNI_LOCK=//p' "$ODI_CONF")"
sh "$S" ack > /dev/null; rm -f "$FLAG" "$COUNTER"
"$F" set OMCI_UNI_LOCK "" > /dev/null

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
t "  no ramlog: nothing says power, so the saved login stays" "login=stored root_password=stored" "$(tr '\n' ' ' < "$FLAG")"
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
t "the third wdt reset: safe mode" "event=safe_mode cause=watchdog short_boots=0 wdt_resets=3 cleared=none root_password=stored prev=wdt_userland login=stored exit_at=1800" "$(cat "$T/kmsg")"
t "  the flag and the record say watchdog" "cause=watchdog.*wdt_resets=3" "$(tr '\n' ' ' < "$RECORD")"
[ ! -f "$WDTFILE" ] && [ ! -f "$COUNTER" ] && ok "  both counters are gone" || no "  both counters are gone"
t "  it spawns the timed exit" "^exit$" "$(spawned)"
[ ! -e "$T/mounts" ] && [ ! -e "$PASSWD_RUN" ] && ok "  root: no bind mount, no default account file" || no "  root: no bind mount, no default account file"
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
t "  and the event does not say root has the default password" "login=default: the web UI asked to accept admin/admin, root NOT on the default password .*root_password=failed" "$(grep 'event=safe_mode ' "$T/kmsg")"
n "  in any wording" "root on the default password admin" "$(grep 'event=safe_mode ' "$T/kmsg")"
sh "$S" ack > /dev/null; rm -f "$FLAG"

# ---- the login: the default credentials only after a power cycle.
# short_to REASON: a safe boot after five short boots, the last ended by REASON.
flagv() { sed -n "s/^$1=//p" "$FLAG" 2>/dev/null; }
short_to() {
	rm -f "$COUNTER" "$WDTFILE" "$RECORD"
	for i in 1 2 3 4 5; do newboot 12; prev power; sh "$S" count > /dev/null; done
	newboot 12; prev "$1"; sh "$S" count > /dev/null
}
short_to power
t "short boots, previous boot power: the default login" "^short_boots default default$" "$(flagv cause) $(flagv login) $(flagv root_password)"
t "  the bind mount is made" "^mount -o bind" "$(cat "$T/mounts" 2>/dev/null)"
# SHORT_UNSTAMPED: short boots whose last one left no stamp (unknown) or no
# ramlog (empty). The one open choice of login_for in safe-mode.sh: default
# today, so a power cut the DRAM survived stays the way back; an unstamped
# reset (a hard hang the hardware watchdog ended) reads the same. Power only
# is that one line removed there and stored here.
SHORT_UNSTAMPED=default
for r in unknown ""; do
	short_to "$r"
	t "short boots, previous boot ${r:-with no ramlog}: the $SHORT_UNSTAMPED login (SHORT_UNSTAMPED)" "^short_boots $SHORT_UNSTAMPED $SHORT_UNSTAMPED$" "$(flagv cause) $(flagv login) $(flagv root_password)"
	t "  and the bind mount goes with it" "^$([ $SHORT_UNSTAMPED = default ] && echo 1 || echo 0)$" "$(grep -c '^mount -o bind' "$T/mounts" 2>/dev/null)"
done
t "  the flag names the reason" "^none$" "$(flagv prev)"
short_to wdt_future_rule
t "short boots, a reason this script does not know: the saved login" "^stored$" "$(flagv login)"
for r in wdt_userland wdt_client:omcid wdt_mem wdt_cpu_rx panic oops reboot halt poweroff; do
	short_to "$r"
	t "short boots, previous boot $r (no power cycle): the saved login" "^short_boots stored stored$" "$(flagv cause) $(flagv login) $(flagv root_password)"
	[ ! -e "$T/mounts" ] && ok "  no bind mount" || no "  no bind mount" "$(cat "$T/mounts")"
done
t "  the event says so" "event=safe_mode cause=short_boots .*root_password=stored prev=poweroff login=stored .*root and ssh as saved, the web UI asked to keep the saved credential" "$(cat "$T/kmsg")"
n "  and does not promise the default password" "default password|admin/admin" "$(grep event=safe_mode "$T/kmsg")"
short_to power
t "after a power cycle the event says what was done for root and what is asked of the web UI" "login=default: root on the default password admin, the web UI asked to accept admin/admin" "$(grep 'event=safe_mode ' "$T/kmsg")"
rm -f "$COUNTER" "$WDTFILE" "$RECORD"

# watchdog: the third reset in a row is the entry, so its reason is never
# power on a stick; a counter already at the limit stands in for the rest.
wdt_to() {
	rm -f "$COUNTER" "$RECORD"; echo 3 > "$WDTFILE"
	newboot 12; prev "$1"; sh "$S" count > /dev/null
}
wdt_to power
t "watchdog cause after a power cycle: the default login" "^watchdog default default$" "$(flagv cause) $(flagv login) $(flagv root_password)"
wdt_to ""
t "watchdog cause, no ramlog: the saved login" "^watchdog stored stored$" "$(flagv cause) $(flagv login) $(flagv root_password)"
for r in wdt_userland panic oops; do
	rm -f "$COUNTER" "$RECORD"; echo 2 > "$WDTFILE"
	newboot 12; prev "$r"; sh "$S" count > /dev/null
	t "watchdog cause, previous boot $r: the saved login" "^watchdog stored stored$" "$(flagv cause) $(flagv login) $(flagv root_password)"
done
rm -f "$COUNTER" "$WDTFILE" "$RECORD"

# config: one power cycle is the way in, a reset of its own is not
newboot 12; prev power; sh "$S" no-config > /dev/null 2>&1
t "config cause after a power cycle: the default login" "^config default default power$" "$(flagv cause) $(flagv login) $(flagv root_password) $(flagv prev)"
t "  the bind mount is made" "^mount -o bind" "$(cat "$T/mounts" 2>/dev/null)"
for r in wdt_userland panic oops reboot halt poweroff unknown ""; do
	newboot 12; prev "$r"; sh "$S" no-config > /dev/null 2>&1
	t "config cause, previous boot ${r:-with no ramlog}: the saved login" "^config stored stored$" "$(flagv cause) $(flagv login) $(flagv root_password)"
	[ ! -e "$T/mounts" ] && ok "  no bind mount" || no "  no bind mount"
done
newboot 12; prev power
: > "$T/not-a-dir"
CONFIG_DIR=$T/not-a-dir COUNTER=$T/not-a-dir/boot-count sh "$S" count > /dev/null 2>&1
t "count with no writable config, after a power cycle: the default login" "^config default$" "$(flagv cause) $(flagv login)"
[ ! -e "$RECORD" ] && ok "  still no record" || no "  still no record"
prev ""

# ---- svc-dropbear.sh on a keys-only image: passwords only with login=default.
# The script names its paths outright, so they are rewritten into the
# temporary tree and respawn.sh is a stub that prints its arguments.
DB=$PWD/rootfs/skeleton/etc/scripts/svc-dropbear.sh
mkdir -p "$T/db/config" "$T/db/run"
echo key > "$T/db/config/ed25519"
printf '#!/bin/sh\necho "$*"\n' > "$T/db/respawn.sh"
: > "$T/db/keys-only"
dropbear_args() {
	sed -e "s|/etc/config/dropbear.d|$T/db/config|g" -e "s|/etc/config/dropbear.off|$T/db/off|" \
	    -e "s|/var/run/safe-mode|$T/db/run/safe-mode|g" -e "s|/etc/odi-keys-only|$T/db/keys-only|" \
	    -e "s|/etc/scripts/respawn.sh|sh $T/db/respawn.sh|" -e "s|\[ -x /sbin/dropbear \]|true|" "$DB" | sh
}
rm -f "$T/db/run/safe-mode"
t "keys-only image, a normal boot: no passwords (-s)" " -s -F " "$(dropbear_args)"
printf 'cause=watchdog\nlogin=stored\nroot_password=stored\n' > "$T/db/run/safe-mode"
t "keys-only image, a safe boot with login=stored: still no passwords" " -s -F " "$(dropbear_args)"
printf 'cause=short_boots\nlogin=default\nroot_password=default\n' > "$T/db/run/safe-mode"
n "keys-only image, a safe boot with login=default: passwords offered" " -s " "$(dropbear_args)"
printf 'cause=short_boots\nxlogin=default\nlogin=defaults\n' > "$T/db/run/safe-mode"
t "  only the exact line counts" " -s -F " "$(dropbear_args)"
: > "$T/db/off"
printf 'cause=watchdog\nlogin=stored\n' > "$T/db/run/safe-mode"
t "  a safe boot with login=stored still starts dropbear whatever dropbear.off says" "^dropbear .* -s -F " "$(dropbear_args)"

# ---- the flag is whole at its one rename: the bind mount comes first, and
# at that moment there is no flag yet, so no reader meets one without its
# login= or exit= line. A flag that cannot be written is no safe mode, and
# the default account file does not outlive it.
cat > "$T/bin/mount-look" <<EOF
#!/bin/sh
[ -e "$FLAG" ] && echo "flag-there" >> "$T/mounts" || echo "no-flag-yet" >> "$T/mounts"
EOF
chmod +x "$T/bin/mount-look"
rm -f "$COUNTER" "$WDTFILE" "$RECORD"
for i in 1 2 3 4 5; do newboot 12; prev power; sh "$S" count > /dev/null; done
newboot 12; prev power; MOUNT=$T/bin/mount-look sh "$S" count > /dev/null
t "the bind mount is made before the flag exists" "^no-flag-yet$" "$(cat "$T/mounts")"
t "  and the flag then has every line at once" "login=default root_password=default exit_at=1800 exit=timed $" "$(tr '\n' ' ' < "$FLAG")"
t "  no temp file is left" "^$" "$(find "$T/run" -name '*.new.*')"
cat > "$T/bin/umount" <<EOF
#!/bin/sh
echo "umount \$*" >> "$T/mounts"
EOF
chmod +x "$T/bin/umount"
t "  the record is the flag without the lines of this boot alone" "prev=power login=default $" "$(tr '\n' ' ' < "$RECORD")"
# The temp file cannot be written (a flag path under a file).
newboot 12; prev power
: > "$T/not-a-dir"
out=$(FLAG=$T/not-a-dir/safe-mode UMOUNT=$T/bin/umount sh "$S" no-config 2>&1); rc=$?
t "a flag that cannot be written: said, not a safe mode, exit 1" "^1 .*event=safe_mode_failed cause=config: could not write" "$rc $out"
n "  no safe_mode event" "event=safe_mode " "$out"
t "  the default account file is taken away again" "umount $PASSWD" "$(cat "$T/mounts")"
t "  and the event says so" "the default root account file was taken away again" "$out"
t "  no exit spawned" "^$" "$(spawned_none)"
# The last step alone fails: the rename onto the flag. mv-noflag refuses
# that one target. The record is published after the flag, so there is none.
mkdir -p "$T/mvbin"
cat > "$T/mvbin/mv" <<EOF
#!/bin/sh
for a in "\$@"; do last=\$a; done
[ "\$last" != "$FLAG" ] || exit 1
exec /bin/mv "\$@"
EOF
chmod +x "$T/mvbin/mv"
rm -f "$COUNTER" "$WDTFILE" "$RECORD"
for i in 1 2 3 4 5; do newboot 12; prev power; sh "$S" count > /dev/null; done
newboot 12; prev power
out=$(PATH="$T/mvbin:$PATH" UMOUNT=$T/bin/umount sh "$S" count 2>&1); rc=$?
t "the final rename fails: count exits 1 and says it is not a safe mode" "^1 .*event=safe_mode_failed cause=short_boots: could not write" "$rc $out"
[ ! -e "$FLAG" ] && [ ! -e "$RECORD" ] && ok "  no flag, and no record of a safe mode that is not one" || no "  no flag, and no record of a safe mode that is not one"
t "  the bind mount is undone, no temp file left, no exit spawned" "^umount $PASSWD  $" "$(grep umount "$T/mounts") $(find "$T/run" "$T/config" -name '*.new.*') $(spawned_none)"
# The same, and the bind mount cannot be undone either: that is said.
newboot 12; prev power
out=$(FLAG=$T/not-a-dir/safe-mode UMOUNT=false sh "$S" no-config 2>&1); rc=$?
t "the flag fails and the umount fails: the event says root is still on the default" "^1 .*event=safe_mode_failed .*could NOT be taken away: root answers to the default password until a reboot" "$rc $out"
n "  and does not claim it was taken away" "was taken away again" "$out"
# A directory where the flag should be: mv would put the temp file inside
# it and answer 0. That is a failure, with the same undoing.
rm -f "$COUNTER" "$WDTFILE" "$RECORD"
for i in 1 2 3 4 5; do newboot 12; prev power; sh "$S" count > /dev/null; done
newboot 12; prev power
mkdir "$FLAG"
out=$(UMOUNT=$T/bin/umount sh "$S" count 2>&1); rc=$?
t "a directory at the flag path: not a safe mode, count exits 1" "^1 .*event=safe_mode_failed cause=short_boots: could not write" "$rc $out"
t "  nothing was moved into it, the bind mount is undone, no record" "^ umount $PASSWD 0$" "$(ls -A "$FLAG") $(grep umount "$T/mounts") $([ -e "$RECORD" ] && echo 1 || echo 0)"
rmdir "$FLAG"
# With login=stored there was no bind mount to undo.
newboot 12; prev wdt_userland
out=$(FLAG=$T/not-a-dir/safe-mode UMOUNT=$T/bin/umount sh "$S" no-config 2>&1)
t "the flag fails with login=stored: nothing to undo, root keeps its own account" "^root keeps its own account $" "$(printf '%s' "$out" | grep -o 'root keeps its own account') $(cat "$T/mounts" 2>/dev/null)"
rm -f "$RECORD" "$COUNTER"

# ---- the timed exit: a safe boot nobody acknowledged reboots at 1800 s
reboots() { cat "$T/reboots" 2>/dev/null; }
export FWU_STATE=$T/fwu.state EXIT_LOCK=$T/run/safe-mode.lock
safe_boot() {
	case $1 in
	short_boots) short_to power ;;
	watchdog)    wdt_to wdt_userland ;;
	config)      newboot 12; prev wdt_userland; sh "$S" no-config > /dev/null 2>&1 ;;
	esac
}
rm -f "$COUNTER" "$WDTFILE" "$RECORD"
for cause in short_boots watchdog config; do
	safe_boot $cause
	t "$cause: the safe boot spawns exit" "^exit$" "$(spawned)"
	: > "$T/kmsg"
	sh "$S" exit > /dev/null
	t "  exit sleeps to 1800 s of uptime" "^1800" "$(cat "$T/uptime")"
	t "  and reboots, once, with busybox reboot" "^reboot $" "$(reboots)"
	t "  and logs it, promising no kind of next boot" "event=safe_mode_exit cause=$cause uptime=1800: nobody acknowledged the safe mode, rebooting$" "$(cat "$T/kmsg")"
	t "  the flag says the reboot has begun" "^exiting$" "$(flagv exit)"
	flock -n "$EXIT_LOCK" true && ok "  the lock is released" || no "  the lock is released"
	# an ack after the timer claimed the flag: refused, the record kept
	had=$([ -e "$RECORD" ] && echo 1 || echo 0)
	: > "$T/kmsg"
	out=$(sh "$S" ack 2>&1); rc=$?
	t "  an ack after the exit began fails" "^1 exiting$" "$rc $(flagv exit)"
	t "  and says the reboot has started" "the timed exit has started and the stick is rebooting" "$out"
	t "  the record is kept, nothing logged as acknowledged" "^$had $" "$([ -e "$RECORD" ] && echo 1 || echo 0) $(cat "$T/kmsg")"
	# a second exit (there is none on a stick) does not reboot twice
	out=$(sh "$S" exit)
	t "  an exit that finds the flag claimed does nothing" "^reboot  .*exit=exiting, not due: no reboot" "$(reboots) $out"
	# the same cause again, acknowledged before the deadline
	safe_boot $cause
	echo "300.00 1.00" > "$T/uptime"; : > "$T/kmsg"
	sh "$S" ack > /dev/null; rc=$?
	t "  ack holds it: exit=held in the flag" "^0 held$" "$rc $(flagv exit)"
	t "  and logs it" "event=safe_mode_hold" "$(cat "$T/kmsg")"
	t "  status says so" "now_exit_at=1800 now_exit=held" "$(sh "$S" status | tr '\n' ' ')"
	[ ! -e "$RECORD" ] && ok "  and the record is gone" || no "  and the record is gone"
	t "  the rest of the flag is kept" "^$cause$" "$(flagv cause)"
	out=$(sh "$S" exit)
	t "  exit after an ack: no reboot" "^$" "$(reboots)"
	t "  and it says why" "acknowledged, no reboot" "$out"
	: > "$T/kmsg"; sh "$S" ack > /dev/null
	t "  a second ack does nothing" "^$" "$(cat "$T/kmsg")"
done
# the ack lands while exit sleeps: the flag is read after the sleep
short_to power
cat > "$T/bin/sleep-ack" <<EOF
#!/bin/sh
"$T/bin/sleep" "\$@"
sh "$S" ack > /dev/null
EOF
chmod +x "$T/bin/sleep-ack"
SLEEP=$T/bin/sleep-ack sh "$S" exit > /dev/null
t "an ack during the wait still holds it" "^1800 $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
# The exit lock is a flock: while another process holds it, ack changes
# nothing and fails, and the exit tries again later, a bounded number of
# times. The real timeout here, since the wait for the lock is the test.
RT=$(PATH=${PATH#"$T/bin:"}; command -v timeout || command -v gtimeout)
if [ -z "$RT" ]; then
	echo "skip  the exit lock held by another process: no timeout or gtimeout on this host"
else
hold_lock() { flock "$EXIT_LOCK" /bin/sleep 30 & HOLDER=$!; /bin/sleep 0.3; }
drop_lock() { pkill -P "$HOLDER" 2>/dev/null; kill "$HOLDER" 2>/dev/null; wait "$HOLDER" 2>/dev/null; }
short_to power
hold_lock
out=$(TIMEOUT=$RT LOCK_S=1 sh "$S" ack 2>&1); rc=$?
t "ack while the lock is held past its wait: fails, nothing changed" "^1 timed 1 .*busy: the exit lock" "$rc $(flagv exit) $([ -e "$RECORD" ] && echo 1 || echo 0) $out"
: > "$T/kmsg"
out=$(TIMEOUT=$RT LOCK_S=1 sh "$S" exit 2>&1); rc=$?
t "exit while the lock is held: three tries, then no reboot" "^1  .*event=safe_mode_exit_failed the exit lock .* was not free in 3 tries: no reboot" "$rc $(reboots) $out"
t "  it slept between the tries, and left the flag alone" "^1920 timed$" "$(cut -d. -f1 "$T/uptime") $(flagv exit)"
drop_lock
sh "$S" ack > /dev/null; rc=$?
t "ack after the holder let go" "^0 held$" "$rc $(flagv exit)"
flock -n "$EXIT_LOCK" true && ok "  and releases the lock itself" || no "  and releases the lock itself"
# a holder that dies leaves nothing behind: the kernel drops the lock
short_to power
hold_lock; drop_lock
sh "$S" exit > /dev/null
t "exit after a holder that died: no stale lock, the reboot" "^reboot $" "$(reboots)"
# the holder lets go between two tries of the exit
short_to power
hold_lock
cat > "$T/bin/sleep-drop" <<EOF
#!/bin/sh
"$T/bin/sleep" "\$@"
pkill -P $HOLDER 2>/dev/null; kill $HOLDER 2>/dev/null
/bin/sleep 0.3
EOF
chmod +x "$T/bin/sleep-drop"
echo "1800.00 1.00" > "$T/uptime"
TIMEOUT=$RT LOCK_S=1 SLEEP=$T/bin/sleep-drop sh "$S" exit > /dev/null 2>&1
t "exit whose first try at the lock failed gets it on the next" "^1860 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
wait "$HOLDER" 2>/dev/null
fi
# no lock file can be made at all: both fail, neither changes anything
short_to power
out=$(EXIT_LOCK=$T/not-a-dir/lock sh "$S" ack 2>&1); rc=$?
t "ack with no lock file to open: fails" "^1 timed$" "$rc $(flagv exit)"
echo "1800.00 1.00" > "$T/uptime"
out=$(EXIT_LOCK=$T/not-a-dir/lock sh "$S" exit 2>&1); rc=$?
t "exit with no lock file to open: no reboot" "^1 timed $" "$rc $(flagv exit) $(reboots)"
# past the deadline already (a late spawn): no sleep, the reboot at once
short_to power
echo "2000.00 1.00" > "$T/uptime"
SLEEP=false sh "$S" exit > /dev/null
t "exit past the deadline reboots without sleeping" "^reboot $" "$(reboots)"
# a sleep that fails, or comes back early, is no deadline
short_to power
out=$(SLEEP=false sh "$S" exit 2>&1); rc=$?
t "a sleep that fails: no reboot, it says so, and the flag stops promising one" "^1 failed .*the wait for 1800 s of uptime failed at 12 s: no reboot" "$rc $(flagv exit) $(reboots) $out"
short_to power
out=$(SLEEP=true sh "$S" exit 2>&1); rc=$?
t "a sleep that returns early every time: no reboot after three" "^1 failed .*uptime 12 s after 3 sleeps, short of 1800 s: no reboot" "$rc $(flagv exit) $(reboots) $out"
cat > "$T/bin/sleep-half" <<EOF
#!/bin/sh
if [ -e "$T/half-done" ]; then exec "$T/bin/sleep" "\$@"; fi
: > "$T/half-done"
"$T/bin/sleep" \$(( \$1 / 2 ))
EOF
chmod +x "$T/bin/sleep-half"
short_to power; rm -f "$T/half-done"
SLEEP=$T/bin/sleep-half sh "$S" exit > /dev/null 2>&1
t "a sleep that returns early once: the uptime is read again, the reboot at the deadline" "^1800 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
# SAFE_EXIT_S is a knob, and the flag carries it
rm -f "$COUNTER" "$RECORD"
for i in 1 2 3 4 5; do newboot 12; sh "$S" count > /dev/null; done
newboot 12; SAFE_EXIT_S=900 sh "$S" count > /dev/null
SAFE_EXIT_S=900 sh "$S" exit > /dev/null
t "SAFE_EXIT_S moves the deadline" "^900 900 reboot $" "$(flagv exit_at) $(cut -d. -f1 "$T/uptime") $(reboots)"
# a reboot that fails is said, and the flag stops promising one
short_to power
out=$(REBOOT=false sh "$S" exit 2>&1); rc=$?
t "a failed reboot is said, as an event" "^1 .*event=safe_mode_exit_failed reboot failed: the safe mode stays up" "$rc $out"
t "  the flag says exit=failed" "^failed$" "$(flagv exit)"
sh "$S" ack > /dev/null; rc=$?
t "  an ack then still removes the record" "^0 0$" "$rc $([ -e "$RECORD" ] && echo 1 || echo 0)"
# not a safe boot: exit does nothing, ack does not make a flag
newboot 12
sh "$S" exit > /dev/null; sh "$S" ack > /dev/null
t "exit on a normal boot: no sleep, no reboot" "^12 $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
[ ! -e "$FLAG" ] && [ ! -e "$EXIT_LOCK" ] && ok "ack on a normal boot makes no flag and no lock" || no "ack on a normal boot makes no flag and no lock"

# ---- the exit waits for a firmware write (the evidence confd refuses a
# reboot on: the state file says running with a live pid, or the writer lock
# is held). sleep-fwu ends the write after two looks.
cat > "$T/bin/sleep-fwu" <<EOF
#!/bin/sh
"$T/bin/sleep" "\$@"
[ "\$1" = 60 ] || exit 0
echo x >> "$T/polls"
[ "\$(wc -l < "$T/polls")" -lt 2 ] || rm -f "$T/fwu.state" "$T/fwu.state.lock"
EOF
chmod +x "$T/bin/sleep-fwu"
short_to power; rm -f "$T/polls"; : > "$T/kmsg"
echo "running $$ slot 1" > "$FWU_STATE"
SLEEP=$T/bin/sleep-fwu sh "$S" exit > /dev/null
t "a write running (state file, live pid): the reboot waits for it" "^1920 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
t "  and says so once" "^1$" "$(grep -c 'event=safe_mode_exit_wait state=postponed' "$T/kmsg")"
short_to power; rm -f "$T/polls"; : > "$T/kmsg"
: > "$FWU_STATE.lock"
flock "$FWU_STATE.lock" /bin/sleep 30 & WRITER=$!
/bin/sleep 0.3
SLEEP=$T/bin/sleep-fwu sh "$S" exit > /dev/null
t "a write running (the writer lock is held): the reboot waits for it" "^1920 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
pkill -P "$WRITER" 2>/dev/null; kill "$WRITER" 2>/dev/null; wait "$WRITER" 2>/dev/null
short_to power
: > "$FWU_STATE.lock"
sh "$S" exit > /dev/null
t "a writer lock nobody holds (the real flock): no wait" "^1800 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
rm -f "$FWU_STATE.lock"
short_to power
echo "running 999999999 slot 1" > "$FWU_STATE"
sh "$S" exit > /dev/null
t "a state left saying running by a pid that is gone: no wait" "^1800 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
echo "done 0" > "$FWU_STATE"
short_to power
sh "$S" exit > /dev/null
t "a write that ended: no wait" "^1800 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
# a write that never ends: bounded, the reboot goes ahead and says so
short_to power; : > "$T/kmsg"
echo "running $$ slot 1" > "$FWU_STATE"
sh "$S" exit > /dev/null
t "a write still running after another 1800 s: the reboot goes ahead" "^3600 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
t "  and says so" "event=safe_mode_exit_wait state=gave_up waited=1800" "$(cat "$T/kmsg")"
# an ack while the exit waits for the write holds it
short_to power
cat > "$T/bin/sleep-fwu-ack" <<EOF
#!/bin/sh
"$T/bin/sleep" "\$@"
[ "\$1" = 60 ] && sh "$S" ack > /dev/null
exit 0
EOF
chmod +x "$T/bin/sleep-fwu-ack"
out=$(SLEEP=$T/bin/sleep-fwu-ack sh "$S" exit)
t "an ack while the exit waits for a write holds it" "^held  .*acknowledged, no reboot" "$(flagv exit) $(reboots) $out"
# a sleep that fails during that wait: no reboot
short_to power
cat > "$T/bin/sleep-poll-fails" <<EOF
#!/bin/sh
[ "\$1" != 60 ] || exit 1
exec "$T/bin/sleep" "\$@"
EOF
chmod +x "$T/bin/sleep-poll-fails"
out=$(SLEEP=$T/bin/sleep-poll-fails sh "$S" exit 2>&1); rc=$?
t "a sleep that fails while waiting for the write: no reboot" "^1 failed  .*the wait for the firmware write failed" "$rc $(flagv exit) $(reboots) $out"
# The wait is measured in uptime, read again after every sleep, never in
# the seconds asked for. A sleep that wakes early during the wait:
cat > "$T/bin/sleep-third" <<EOF
#!/bin/sh
exec "$T/bin/sleep" \$(( (\$1 + 2) / 3 ))
EOF
chmod +x "$T/bin/sleep-third"
short_to power; : > "$T/kmsg"
echo "1800.00 1.00" > "$T/uptime"
echo "running $$ slot 1" > "$FWU_STATE"
SLEEP=$T/bin/sleep-third sh "$S" exit > /dev/null
t "sleeps that wake early while a write runs: the reboot not before the uptime deadline" "^3600 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
t "  and the wait it reports is the uptime that passed" "state=gave_up waited=1800" "$(cat "$T/kmsg")"
# a sleep that never advances the uptime: bounded, and no reboot
short_to power
echo "1800.00 1.00" > "$T/uptime"
out=$(SLEEP=true sh "$S" exit 2>&1); rc=$?
t "sleeps that do not advance the uptime while a write runs: bounded, no reboot" "^1 failed  .*did not advance in 124 sleeps" "$rc $(flagv exit) $(reboots) $out"
# a late start: the wait for the write runs from when it began, not from
# the deadline of the safe mode
short_to power; : > "$T/kmsg"
echo "2000.00 1.00" > "$T/uptime"
sh "$S" exit > /dev/null
t "a late start: the write gets its whole wait, from the uptime it began at" "^3800 reboot .*until uptime 3800 s at most" "$(cut -d. -f1 "$T/uptime") $(reboots) $(grep postponed "$T/kmsg")"
# an interval that does not divide the wait: the last sleep is the rest
rm -f "$COUNTER" "$RECORD"
for i in 1 2 3 4 5; do newboot 12; sh "$S" count > /dev/null; done
newboot 12; SAFE_EXIT_S=100 sh "$S" count > /dev/null
: > "$T/kmsg"; : > "$T/sleeps"
cat > "$T/bin/sleep-log" <<EOF
#!/bin/sh
echo "\$1" >> "$T/sleeps"
exec "$T/bin/sleep" "\$@"
EOF
chmod +x "$T/bin/sleep-log"
SAFE_EXIT_S=100 EXIT_POLL_S=60 SLEEP=$T/bin/sleep-log sh "$S" exit > /dev/null
t "an interval that does not divide the wait: 60 then the remaining 40, not 120" "^88 60 40 200 reboot $" "$(tr '\n' ' ' < "$T/sleeps")$(cut -d. -f1 "$T/uptime") $(reboots)"
t "  waited=100" "state=gave_up waited=100" "$(cat "$T/kmsg")"
# the write is looked at under the lock, right before the claim: one that
# starts during the last sleep of the wait to the deadline is still seen
short_to power
cat > "$T/bin/sleep-startfwu" <<EOF
#!/bin/sh
"$T/bin/sleep" "\$@"
[ -e "$T/started" ] || { : > "$T/started"; echo "running $$ slot 1" > "$FWU_STATE"; exit 0; }
rm -f "$FWU_STATE"
EOF
chmod +x "$T/bin/sleep-startfwu"
rm -f "$T/started" "$FWU_STATE"
SLEEP=$T/bin/sleep-startfwu sh "$S" exit > /dev/null
t "a write that starts just before the deadline is waited for" "^1860 reboot $" "$(cut -d. -f1 "$T/uptime") $(reboots)"
rm -f "$FWU_STATE" "$FWU_STATE.lock"
# From the claim through the reboot the exit holds the writer lock, the one
# fwu_starter.sh takes before it touches anything: a write that asks to
# start then is refused. The stub reboot runs the real starter.
STARTER=$PWD/rootfs/skeleton/etc/scripts/fwu_starter.sh
cat > "$T/bin/reboot-starter" <<EOF
#!/bin/sh
echo "reboot" >> "$T/reboots"
: > "$T/img.tar"
sh "$STARTER" 1 "$T/img.tar" > "$T/starter.out" 2>&1
echo "rc=\$?" >> "$T/starter.out"
EOF
chmod +x "$T/bin/reboot-starter"
short_to power
REBOOT=$T/bin/reboot-starter sh "$S" exit > /dev/null
t "a write asked for after the exit claimed the flag is refused by fwu_starter.sh" "^reboot .*a write is already running.*rc=1" "$(reboots) $(tr '\n' ' ' < "$T/starter.out")"
[ ! -e "$FWU_STATE" ] && ok "  and nothing of a write was begun" || no "  and nothing of a write was begun"
flock -n "$FWU_STATE.lock" true && ok "  the writer lock is free once the exit is gone" || no "  the writer lock is free once the exit is gone"
# a reboot that fails gives the writer lock back
short_to power
cat > "$T/bin/reboot-fails" <<EOF
#!/bin/sh
exit 1
EOF
chmod +x "$T/bin/reboot-fails"
REBOOT=$T/bin/reboot-fails sh "$S" exit > /dev/null 2>&1
flock -n "$FWU_STATE.lock" true && ok "a reboot that fails leaves the writer lock free" || no "a reboot that fails leaves the writer lock free"
# no place for the writer lock file: nothing can hold it, the reboot goes on
short_to power
FWU_STATE=$T/not-a-dir/fwu.state sh "$S" exit > /dev/null 2>&1
t "no writer lock file can be made: the reboot goes ahead" "^reboot $" "$(reboots)"
rm -f "$FWU_STATE" "$FWU_STATE.lock"
# An uptime that cannot be read is never taken for 0. Only the first read
# of the wait for a write fails here: flock-busy says the writer lock is
# held and spoils the uptime file at that moment.
cat > "$T/bin/flock-busy" <<EOF
#!/bin/sh
if [ "\$1" = -n ] && [ "\$2" = 8 ]; then echo garbage > "$T/uptime"; exit 1; fi
exec flock "\$@"
EOF
chmod +x "$T/bin/flock-busy"
short_to power
echo "1800.00 1.00" > "$T/uptime"
out=$(FLOCK=$T/bin/flock-busy sh "$S" exit 2>&1); rc=$?
t "the first uptime read of the wait for a write fails: no reboot, exit=failed" "^1 failed  .*the uptime could not be read while a firmware write runs" "$rc $(flagv exit) $(reboots) $out"
n "  and nothing was computed from 0" "until uptime 1800 s" "$out"
short_to power
echo garbage > "$T/uptime"
out=$(sh "$S" exit 2>&1); rc=$?
t "the uptime cannot be read at the start: no sleep, no reboot" "^1 failed  .*the uptime could not be read" "$rc $(flagv exit) $(reboots) $out"
echo "12.00 1.00" > "$T/uptime"

# ---- the boot after the exit: reason=reboot, a normal boot from zero, the
# record still there to say what happened
wdt_to wdt_userland
sh "$S" exit > /dev/null
boot_after reboot 12
sh "$S" active && no "the boot after a timed exit is normal" || ok "the boot after a timed exit is normal"
t "  with no watchdog count, and counted from 1" "^ 1$" "$(wdtn) $(count)"
t "  the record waits for an acknowledgement" "record=1 record_cause=watchdog" "$(sh "$S" status | tr '\n' ' ')"
t "  and boot-ok is spawned as on any normal boot" "boot-ok" "$(spawned)"
sh "$S" ack > /dev/null
rm -f "$COUNTER" "$WDTFILE" "$RECORD" "$PREV"
# The safe boot removed the counter itself, so the case above does not show
# that reason=reboot clears one. Here a count of 2 survives into the boot,
# as after two resets and then a reboot of any kind, the timed exit included.
echo 2 > "$WDTFILE"
boot_after reboot 12
t "a watchdog count of 2 is cleared by reason=reboot" "^ wdt_before=0 $" "$(wdtn) $(grep -o 'wdt_before=[0-9]*' "$T/kmsg") $([ -e "$FLAG" ] && echo safe)"
rm -f "$COUNTER"
echo 2 > "$WDTFILE"
boot_after wdt_userland 12
t "  the same count and one more reset would have been the safe mode" "^watchdog$" "$(flagv cause)"
sh "$S" ack > /dev/null
rm -f "$COUNTER" "$WDTFILE" "$RECORD" "$PREV" "$FLAG"

# ---- rcs-lib.sh: the PON steps a safe-mode boot skips
pon() {
	(
		# shellcheck source=/dev/null
		. "$LIB"
		# shellcheck disable=SC2034 # read by the sourced functions
		SAFE_FLAG=$1
		# shellcheck disable=SC2034 # read by the sourced functions
		HS=$T/hs.xml
		crumb() { echo "crumb $*"; }
		pon_step() { echo "step $1"; }
		pon_step_laser
		pon_step_gponact
	) 2>&1
}
printf '<Value Name="GPON_SN" Value="ODIT00000001"/>\n' > "$T/hs.xml"
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
