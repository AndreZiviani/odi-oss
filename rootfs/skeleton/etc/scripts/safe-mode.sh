#!/bin/sh
#
# safe-mode.sh -- the recovery boot: a way back into a stick that cannot be
# reached or logged into (a host port mode the router does not speak, a
# forgotten web UI password, lost ssh keys), with nothing but its power.
#
#     safe-mode.sh count     rcS, once, right after the config partition is
#                            mounted: count the boot, and enter the safe mode
#                            when the boots before it were short or ended in
#                            a watchdog reset
#     safe-mode.sh no-config rcS, instead of count, when the config partition
#                            is not mounted: the safe mode, with no record
#     safe-mode.sh boot-ok   spawned by count: clears the short-boot counter
#                            at BOOT_OK_S (60) of uptime and the watchdog
#                            counter at WDT_OK_S (600)
#     safe-mode.sh ack       acknowledge a safe mode: removes its record
#     safe-mode.sh status    key=value lines: this boot, and the record
#     safe-mode.sh active    exit 0 when this boot is a safe-mode boot
#
# The trigger. Every boot adds one to a counter on the config partition as
# early as the partition allows, and a boot that reaches BOOT_OK_S of uptime
# clears it. When the boots before this one were all short -- SAFE_BOOTS_AN_OFF
# (3) of them with HOST_AUTONEG=off, SAFE_BOOTS (5) otherwise -- this boot is
# a safe-mode boot. So: plug the stick in, wait about 80 s, pull it; repeat;
# then leave it in: the fourth (or sixth) boot is the safe one. Uptime
# starts at the kernel, about 54 s after a cold power-on on the sticks
# measured, so a power-up counts between about 60 and 110 s after
# power-on. Each count logs its uptime, which with the board boot time
# gives that window.
#
# The second trigger is a reset loop the first one cannot see. A boot whose
# userland never confirms (odi_wdt resets it at 123 s) or whose client stops
# pinging is not short: it clears the counter at 60 s, and loops for ever.
# The kernel records why every boot ended (reason= in /proc/odi_ramlog_prev,
# which survives a watchdog reset); a reason of wdt_* (a watchdog rule fired),
# panic or oops adds one to a second counter, /etc/config/boot-wdt. reboot,
# halt, poweroff and unknown (a stock boot in between, an emergency restart)
# clear it, power (DRAM lost: a power cycle) leaves it, and a boot that
# lives WDT_OK_S (600) clears it too, so a rare reset hours apart never adds
# up. WDT_BOOTS (3) such boots in a row make this boot a safe-mode boot. 3,
# not 2: one legitimate reset, or two close together, must not trip it; the
# loops it is for repeat every 2-3 minutes, so it fires in about ten.
#
# The third is a config partition that is not mounted, or not writable
# (rcS calls no-config): nothing can be counted and the stored address,
# credential and keys are unreadable, so the boot is a safe-mode boot at
# once. It leaves the next boot alone, which tries the partition again.
#
# The safe mode lasts this one boot, and writes nothing to the config
# partition but its record (and, when HOST_AUTONEG=off made it fire, the
# removal of that key, which is the setting that needed undoing):
#   - the host port runs auto-negotiation (host-autoneg.sh does not apply
#     HOST_AUTONEG this boot);
#   - the PON is off: the PON steps leave the laser off and skip gponact
#     (rcs-lib.sh), so the stick cannot disturb the OLT, and the kernel
#     refuses an activation with the laser off anyway;
#   - the web UI accepts the built-in admin:admin (confd reads the flag
#     below) and root over ssh accepts the password admin (an /etc/passwd of
#     this boot, bind-mounted over the read-only one, and dropbear offering
#     passwords). Nothing stored is changed: the next normal boot uses the
#     saved credential and keys again. A password set in the web UI during
#     the safe mode is saved, and in force from the next boot.
#   - the boot is confirmed to the kernel watchdog (rcs-lib.sh) without the
#     mount, address and confirm-arp checks, so the safe boot stays up for
#     a person to reach, even with the config partition dead; the switch
#     init must still have worked. It cannot loop: the next boot is a
#     normal one, and a fault that persists takes WDT_BOOTS more resets to
#     bring the safe mode back, which then stays up until someone reboots.
# The flag of this boot is /var/run/safe-mode (tmpfs: gone at the next
# boot); the record is /etc/config/safe-mode, kept until acknowledged so the
# web UI can say it happened. Physical access is enough to reset the
# credential this way, for one boot: that is the trade-off, documented in
# docs/SETTINGS.md.
#
# Every wait is bounded and every file is written by temp file and rename.
set -u

# confd starts its children with no environment; export a PATH so that
# commands started through busybox timeout find /sbin and /usr/sbin too.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH

FLASH=${FLASH:-/etc/scripts/flash}
SELF=${SELF:-/etc/scripts/safe-mode.sh}
COUNTER=${COUNTER:-/etc/config/boot-count}
WDTFILE=${WDTFILE:-/etc/config/boot-wdt}
PREV=${PREV:-/proc/odi_ramlog_prev}
RECORD=${RECORD:-/etc/config/safe-mode}
FLAG=${FLAG:-/var/run/safe-mode}
PASSWD=${PASSWD:-/etc/passwd}
PASSWD_RUN=${PASSWD_RUN:-/var/run/safe-mode.passwd}
CONFIG_DIR=${CONFIG_DIR:-/etc/config}
UPTIME=${UPTIME:-/proc/uptime}
KMSG=${KMSG:-/dev/kmsg}
BUILD=${BUILD:-/etc/odi-build}
LOG=${LOG:-/var/log/services.log}
SLEEP=${SLEEP:-sleep}
TIMEOUT=${TIMEOUT:-timeout}
MOUNT=${MOUNT:-mount}

BOOT_OK_S=${BOOT_OK_S:-60}             # uptime that makes a boot not short
SAFE_BOOTS=${SAFE_BOOTS:-5}            # short boots before the safe one
SAFE_BOOTS_AN_OFF=${SAFE_BOOTS_AN_OFF:-3}  # the same, with HOST_AUTONEG=off
WDT_BOOTS=${WDT_BOOTS:-3}              # boots ended by a watchdog reset in a row
WDT_OK_S=${WDT_OK_S:-600}              # uptime that clears the watchdog counter
IO_S=${IO_S:-5}                        # one flash or mount call

# root:admin for this boot, SHA-512 crypt as image/build.sh writes it (the
# hash of a public default is not a secret).
ROOT_SAFE='root:$6$odisafemode$y21AxHRooaPWf.3Es9wIY2QMhn985CsQHyhfHN9SuqzwUwNPDD3wI23gjjBYGSQ.4tkSYeESNrK4lN/Q03Q93/:0:0:root:/root:/bin/sh'

say() { echo "safe-mode: $*"; }
uptime_s() { cut -d. -f1 "$UPTIME" 2>/dev/null; }

# event NAME TEXT: one line to the kernel log (dmesg, the ramlog, syslog
# through klogd), at notice, and to stdout.
event() {
	name=$1
	shift
	printf '<5>safe-mode: event=%s %s\n' "$name" "$*" 2>/dev/null >> "$KMSG"
	say "event=$name $*"
}

# put FILE: FILE.new.$$ in place of FILE in one rename.
put() {
	mv "$1.new.$$" "$1" 2>/dev/null || { rm -f "$1.new.$$"; return 1; }
}

key() {
	"$TIMEOUT" "$IO_S" "$FLASH" get HOST_AUTONEG 2>/dev/null | sed -n 's/^HOST_AUTONEG=//p' | head -n 1
}

# prev_reason: why the previous boot ended, as the kernel recorded it
# (odi_ramlog.c, "previous boot: ... reason=wdt_userland"); empty when the
# file is not there (an older kernel).
prev_reason() {
	sed -n 's/^previous boot:.* reason=\([^ ]*\).*/\1/p' "$PREV" 2>/dev/null | head -n 1
}

# wdt_update REASON: the watchdog counter after the previous boot ended with
# REASON, stored in WDTFILE and printed. Written only when it changes.
wdt_update() {
	w=$(sed -n '1s/^\([0-9][0-9]*\)$/\1/p' "$WDTFILE" 2>/dev/null)
	w=${w:-0}
	case "$1" in
	wdt_*|panic|oops) new=$(( w + 1 )) ;;
	power|'') new=$w ;;
	*) new=0 ;;
	esac
	if [ "$new" != "$w" ]; then
		if [ "$new" = 0 ]; then
			rm -f "$WDTFILE"
		else
			{ echo "$new" > "$WDTFILE.new.$$" && put "$WDTFILE"; } || say "could not write $WDTFILE" >&2
		fi
	fi
	echo "$new"
}

count() {
	if [ ! -d "$CONFIG_DIR" ] || [ ! -w "$CONFIG_DIR" ]; then
		no_config
		return 0
	fi
	n=$(sed -n '1s/^\([0-9][0-9]*\)$/\1/p' "$COUNTER" 2>/dev/null)
	n=$(( ${n:-0} + 1 ))
	up=$(uptime_s)
	if ! { echo "$n" > "$COUNTER.new.$$" && put "$COUNTER"; }; then
		no_config
		return 0
	fi
	reason=$(prev_reason)
	wdt=$(wdt_update "$reason")
	an=$(key)
	limit=$SAFE_BOOTS
	[ "$an" = off ] && limit=$SAFE_BOOTS_AN_OFF
	short=$(( n - 1 ))
	event boot_count "boot=$n short_before=$short limit=$limit wdt_before=$wdt wdt_limit=$WDT_BOOTS prev=${reason:-none} uptime=$up"
	if [ "$short" -ge "$limit" ]; then
		enter short_boots "$short" "$wdt" "$an"
		return 0
	fi
	if [ "$wdt" -ge "$WDT_BOOTS" ]; then
		enter watchdog "$short" "$wdt" "$an"
		return 0
	fi
	setsid sh "$SELF" boot-ok < /dev/null >> "$LOG" 2>&1 &
	return 0
}

# no_config: the config partition is not mounted or not writable. Nothing
# can be counted, so this boot is a safe-mode boot, with no record (there is
# nowhere to write it; the flag in /var/run still says it).
no_config() {
	say "no writable $CONFIG_DIR: safe mode for this boot" >&2
	enter config 0 0 ""
}

# enter CAUSE SHORT WDT HOST_AUTONEG: the safe mode, for this boot. CAUSE is
# short_boots, watchdog or config.
enter() {
	cause=$1 short=$2 wdt=$3 an=$4 cleared=none
	if [ "$cause" = short_boots ] && [ "$an" = off ]; then
		"$TIMEOUT" "$IO_S" "$FLASH" set HOST_AUTONEG "" > /dev/null 2>&1
		if [ -z "$(key)" ]; then cleared=HOST_AUTONEG; else cleared=failed; fi
	fi
	rm -f "$COUNTER" "$WDTFILE"
	{
		echo "cause=$cause"
		echo "short_boots=$short"
		echo "wdt_resets=$wdt"
		echo "uptime=$(uptime_s)"
		echo "cleared=$cleared"
		echo "build=$(sed -n 's/^image=//p' "$BUILD" 2>/dev/null | head -n 1)"
	} > "$FLAG.new.$$" && put "$FLAG"
	[ "$cause" = config ] || { cp "$FLAG" "$RECORD.new.$$" 2>/dev/null && put "$RECORD"; }
	# The root password of this boot: a copy of the account file in RAM,
	# bind-mounted over the read-only one. Nothing on flash changes.
	pw=failed
	if echo "$ROOT_SAFE" > "$PASSWD_RUN" && chmod 644 "$PASSWD_RUN" &&
	   "$TIMEOUT" "$IO_S" "$MOUNT" -o bind "$PASSWD_RUN" "$PASSWD" 2>/dev/null; then
		pw=default
	fi
	echo "root_password=$pw" >> "$FLAG"
	event safe_mode "cause=$cause short_boots=$short wdt_resets=$wdt cleared=$cleared root_password=$pw: this boot only, the PON off, the host port on auto-negotiation, the web UI and root on the default password admin"
}

boot_ok() {
	up=$(uptime_s)
	[ "${up:-0}" -ge "$BOOT_OK_S" ] || "$SLEEP" $(( BOOT_OK_S - ${up:-0} ))
	if [ -f "$COUNTER" ]; then
		rm -f "$COUNTER"
		say "up ${BOOT_OK_S} s: the boot counter is cleared"
	fi
	# The watchdog counter outlives this: a loop of unconfirmed boots dies at
	# 123 s, so a boot that lives WDT_OK_S has ended it. Nothing to wait for
	# when the previous boots did not end in a reset.
	[ -f "$WDTFILE" ] || return 0
	up=$(uptime_s)
	[ "${up:-0}" -ge "$WDT_OK_S" ] || "$SLEEP" $(( WDT_OK_S - ${up:-0} ))
	rm -f "$WDTFILE"
	say "up ${WDT_OK_S} s: the watchdog counter is cleared"
}

ack() {
	if [ -f "$RECORD" ]; then
		rm -f "$RECORD"
		event safe_mode_ack "the safe-mode record was acknowledged"
	fi
	return 0
}

status() {
	if [ -f "$FLAG" ]; then
		echo "active=1"
		sed -n 's/^\([a-z_]*\)=/now_\1=/p' "$FLAG"
	else
		echo "active=0"
	fi
	if [ -f "$RECORD" ]; then
		echo "record=1"
		sed -n 's/^\([a-z_]*\)=/record_\1=/p' "$RECORD"
	else
		echo "record=0"
	fi
	[ -f "$COUNTER" ] && echo "boot_count=$(cat "$COUNTER")"
	[ -f "$WDTFILE" ] && echo "wdt_count=$(cat "$WDTFILE")"
	return 0
}

case "${1:-}" in
count)   count ;;
no-config) no_config ;;
boot-ok) boot_ok ;;
ack)     ack ;;
status)  status ;;
active)  [ -f "$FLAG" ] ;;
*)       echo "usage: $0 count|no-config|boot-ok|ack|status|active" >&2; exit 1 ;;
esac
