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
# the laser and gponact PON steps skipped on a safe-mode boot only.
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
spawned() { /bin/sleep 0.3; cat "$T/spawned" 2>/dev/null; }
count() { cat "$COUNTER" 2>/dev/null; }
config_files() { (cd "$T/config" && ls -A | sort | tr '\n' ' '); }

# ---- counting, default limit 5
setkey ""
for i in 1 2 3 4 5; do
	newboot 12
	sh "$S" count > /dev/null
	t "boot $i: counted $i" "^$i$" "$(count)"
done
t "the count logs the boot, the limit and the uptime" "event=boot_count boot=5 short_before=4 limit=5 uptime=12" "$(cat "$T/kmsg")"
t "a counted boot spawns boot-ok" "boot-ok" "$(spawned)"
[ ! -f "$FLAG" ] && ok "four short boots before: not yet" || no "four short boots before: not yet"
newboot 14
sh "$S" count > /dev/null
t "five short boots before: safe mode" "event=safe_mode short_boots=5 cleared=none root_password=default" "$(cat "$T/kmsg")"
t "  the flag of this boot" "short_boots=5.*cleared=none.*build=test-image.*root_password=default" "$(tr '\n' ' ' < "$FLAG")"
t "  the record on the config partition" "short_boots=5.*cleared=none" "$(tr '\n' ' ' < "$RECORD")"
[ ! -f "$COUNTER" ] && ok "  the counter is gone" || no "  the counter is gone"
t "  the only config change is the record (and the counter removed)" "^safe-mode $" "$(config_files | sed 's/odi.conf //')"
t "  root: the RAM copy is bind-mounted over the account file" "^mount -o bind $PASSWD_RUN $PASSWD$" "$(cat "$T/mounts")"
t "  root: the RAM copy carries the default password hash" '^root:\$6\$odisafemode\$[^:]+:0:0:root:/root:/bin/sh$' "$(cat "$PASSWD_RUN")"
t "  the account file on flash is untouched" '^root:!:0:0' "$(cat "$PASSWD")"
t "  nothing spawned" "^$" "$(spawned)"
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
t "key off: the fourth boot is the safe one" "event=safe_mode short_boots=3 cleared=HOST_AUTONEG" "$(cat "$T/kmsg")"
t "  HOST_AUTONEG is cleared" "^$" "$(key)"
sh "$S" ack > /dev/null

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

# ---- no writable config partition: not counted, no safe mode
chmod 500 "$T/config"
newboot 12
out=$(sh "$S" count 2>&1)
t "no writable config: not counted" "not counted" "$out"
chmod 700 "$T/config"

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

echo
echo "safe_mode_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
