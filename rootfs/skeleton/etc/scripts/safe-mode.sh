#!/bin/sh
#
# safe-mode.sh -- the recovery boot: a way back into a stick that cannot be
# reached or logged into (a host port mode the router does not speak, a
# forgotten web UI password, lost ssh keys), with nothing but its power.
#
#     safe-mode.sh count     rcS, once, right after the config partition is
#                            mounted: count the boot, and enter the safe mode
#                            when the boots before it were short
#     safe-mode.sh boot-ok   spawned by count: clears the counter once the
#                            uptime reaches BOOT_OK_S (60)
#     safe-mode.sh ack       acknowledge a safe mode: removes its record
#     safe-mode.sh status    key=value lines: this boot, and the record
#     safe-mode.sh active    exit 0 when this boot is a safe-mode boot
#
# The trigger. Every boot adds one to a counter on the config partition as
# early as the partition allows, and a boot that reaches BOOT_OK_S of uptime
# clears it. When the boots before this one were all short -- SAFE_BOOTS_AN_OFF
# (3) of them with HOST_AUTONEG=off, SAFE_BOOTS (5) otherwise -- this boot is
# a safe-mode boot. So: plug the stick in, count to 30, unplug; repeat; the
# fourth (or sixth) boot is the safe one. Each count logs its uptime, which
# is what bounds the window on hardware: power has to stay on past the
# count, and go off before 60 s.
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
# The flag of this boot is /var/run/safe-mode (tmpfs: gone at the next
# boot); the record is /etc/config/safe-mode, kept until acknowledged so the
# web UI can say it happened. Physical access is enough to reset the
# credential this way, for one boot: that is the trade-off, documented in
# docs/SETTINGS.md.
#
# Every wait is bounded and every file is written by temp file and rename.
set -u

FLASH=${FLASH:-/etc/scripts/flash}
SELF=${SELF:-/etc/scripts/safe-mode.sh}
COUNTER=${COUNTER:-/etc/config/boot-count}
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

count() {
	if [ ! -d "$CONFIG_DIR" ] || [ ! -w "$CONFIG_DIR" ]; then
		say "no writable $CONFIG_DIR: this boot is not counted" >&2
		return 0
	fi
	n=$(sed -n '1s/^\([0-9][0-9]*\)$/\1/p' "$COUNTER" 2>/dev/null)
	n=$(( ${n:-0} + 1 ))
	up=$(uptime_s)
	if ! { echo "$n" > "$COUNTER.new.$$" && put "$COUNTER"; }; then
		say "could not write $COUNTER: this boot is not counted" >&2
		return 0
	fi
	an=$(key)
	limit=$SAFE_BOOTS
	[ "$an" = off ] && limit=$SAFE_BOOTS_AN_OFF
	short=$(( n - 1 ))
	event boot_count "boot=$n short_before=$short limit=$limit uptime=$up"
	if [ "$short" -lt "$limit" ]; then
		setsid sh "$SELF" boot-ok < /dev/null >> "$LOG" 2>&1 &
		return 0
	fi
	enter "$short" "$an"
}

# enter SHORT HOST_AUTONEG: the safe mode, for this boot.
enter() {
	short=$1 an=$2 cleared=none
	if [ "$an" = off ]; then
		"$TIMEOUT" "$IO_S" "$FLASH" set HOST_AUTONEG "" > /dev/null 2>&1
		if [ -z "$(key)" ]; then cleared=HOST_AUTONEG; else cleared=failed; fi
	fi
	rm -f "$COUNTER"
	{
		echo "short_boots=$short"
		echo "uptime=$(uptime_s)"
		echo "cleared=$cleared"
		echo "build=$(sed -n 's/^image=//p' "$BUILD" 2>/dev/null | head -n 1)"
	} > "$FLAG.new.$$" && put "$FLAG"
	cp "$FLAG" "$RECORD.new.$$" 2>/dev/null && put "$RECORD"
	# The root password of this boot: a copy of the account file in RAM,
	# bind-mounted over the read-only one. Nothing on flash changes.
	pw=failed
	if echo "$ROOT_SAFE" > "$PASSWD_RUN" && chmod 644 "$PASSWD_RUN" &&
	   "$TIMEOUT" "$IO_S" "$MOUNT" -o bind "$PASSWD_RUN" "$PASSWD" 2>/dev/null; then
		pw=default
	fi
	echo "root_password=$pw" >> "$FLAG"
	event safe_mode "short_boots=$short cleared=$cleared root_password=$pw: this boot only, the PON off, the host port on auto-negotiation, the web UI and root on the default password admin"
}

boot_ok() {
	up=$(uptime_s)
	[ "${up:-0}" -ge "$BOOT_OK_S" ] || "$SLEEP" $(( BOOT_OK_S - ${up:-0} ))
	if [ -f "$COUNTER" ]; then
		rm -f "$COUNTER"
		say "up ${BOOT_OK_S} s: the boot counter is cleared"
	fi
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
	return 0
}

case "${1:-}" in
count)   count ;;
boot-ok) boot_ok ;;
ack)     ack ;;
status)  status ;;
active)  [ -f "$FLAG" ] ;;
*)       echo "usage: $0 count|ack|status|active" >&2; exit 1 ;;
esac
