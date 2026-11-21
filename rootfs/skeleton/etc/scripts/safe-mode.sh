#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

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
#     safe-mode.sh exit      spawned by a safe-mode boot: reboots it at
#                            SAFE_EXIT_S (1800) of uptime, unless it was
#                            acknowledged
#     safe-mode.sh ack       acknowledge a safe mode: removes its record, and
#                            keeps a safe-mode boot up past SAFE_EXIT_S
#     safe-mode.sh status    key=value lines: this boot, and the record
#     safe-mode.sh active    exit 0 when this boot is a safe-mode boot
#
# The trigger. Every boot adds one to a counter on the config partition as
# early as the partition allows, and a boot that reaches BOOT_OK_S of uptime
# clears it. When the boots before this one were all short -- SAFE_BOOTS_AN_OFF
# (3) of them with HOST_AUTONEG=off or HOST_SERDES set to 2500basex or
# hisgmii, SAFE_BOOTS (5) otherwise -- this boot is a safe-mode boot. So: plug the stick in, wait about 80 s, pull it; repeat;
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
# partition but its record (and, when HOST_AUTONEG=off or a HOST_SERDES
# mode other than fiber1g made it fire, the removal of that key, which is
# the setting that needed undoing):
#   - the host port runs Fiber 1G (1000BASE-X), the most compatible host
#     mode, with auto-negotiation (host-serdes.sh does not apply
#     HOST_SERDES this boot, host-autoneg.sh not HOST_AUTONEG);
#   - the stored static routes (LAN_ROUTES) are not applied: network.sh skips
#     them when this flag is set. The rescue address below is the way in, and a
#     stored route is one of the things that may need repair;
#   - the PON is off: the PON steps leave the laser off and skip gponact
#     (rcs-lib.sh), so the stick cannot disturb the OLT, and the kernel
#     refuses an activation with the laser off anyway;
#   - after a power cycle only (login=default in the flag), the web UI
#     accepts the built-in admin:admin (confd reads the flag below) and root
#     over ssh accepts the password admin (an /etc/passwd of this boot,
#     bind-mounted over the read-only one, and dropbear offering passwords).
#     Nothing stored is changed: the next normal boot uses the saved
#     credential and keys again. A password set in the web UI during the
#     safe mode is saved, and in force from the next boot. A safe mode
#     nobody used the power for (login=stored) opens nothing: the web UI
#     keeps the saved credential, root its account and dropbear its keys.
#     Which one it is comes from why the previous boot ended (login_for);
#   - it ends by itself: at SAFE_EXIT_S (1800) of uptime the boot reboots,
#     so a stick nobody is looking at tries its PON again and the default
#     password does not stay open. An acknowledgement (the web UI button,
#     safe-mode.sh ack) says someone is there, and the boot then stays up
#     until that person reboots it. The reboot waits for a firmware write
#     that is running (safe_exit). What boots next is what any reboot
#     boots: the committed slot, or once a trial that is armed; it is a
#     safe-mode boot again only when the config partition is still
#     unusable;
#   - the boot is confirmed to the kernel watchdog (rcs-lib.sh) without the
#     mount, address and confirm-arp checks, so the safe boot stays up for
#     a person to reach, even with the config partition dead; the switch
#     init must still have worked. It cannot loop: the next boot is a
#     normal one unless the config partition is still unusable, and a
#     watchdog fault that persists takes WDT_BOOTS more resets to bring the
#     safe mode back, for SAFE_EXIT_S again.
# The flag of this boot is /var/run/safe-mode (tmpfs: gone at the next
# boot); the record is /etc/config/safe-mode, kept until acknowledged so the
# web UI can say it happened. Physical access is enough to reset the
# credential this way, for one boot: that is the trade-off, documented in
# docs/SETTINGS.md. No reset the stick does to itself is.
#
# Every wait is bounded and every file is written by temp file and rename.
# The flag is complete at its one rename: no reader sees it without its
# login= line or its exit= line.
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
UMOUNT=${UMOUNT:-umount}
REBOOT=${REBOOT:-reboot}
FLOCK=${FLOCK:-flock}
EXIT_LOCK=${EXIT_LOCK:-$FLAG.lock}
FWU_STATE=${FWU_STATE:-/tmp/fwu.state}

BOOT_OK_S=${BOOT_OK_S:-60}             # uptime that makes a boot not short
SAFE_BOOTS=${SAFE_BOOTS:-5}            # short boots before the safe one
SAFE_BOOTS_AN_OFF=${SAFE_BOOTS_AN_OFF:-3}  # the same, with HOST_AUTONEG=off
WDT_BOOTS=${WDT_BOOTS:-3}              # boots ended by a watchdog reset in a row
WDT_OK_S=${WDT_OK_S:-600}              # uptime that clears the watchdog counter
SAFE_EXIT_S=${SAFE_EXIT_S:-1800}       # uptime at which a safe boot nobody acknowledged reboots
EXIT_POLL_S=${EXIT_POLL_S:-60}         # between two looks at a firmware write the exit waits for
LOCK_S=${LOCK_S:-5}                    # waiting for the exit lock, one try
EXIT_LOCK_TRIES=${EXIT_LOCK_TRIES:-3}  # tries of the timed exit at that lock, EXIT_POLL_S apart
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

# put FILE: FILE.new.$$ in place of FILE in one rename. A directory where
# FILE should be is a failure: mv would move the temp file into it and
# answer 0, with nothing at FILE to read.
put() {
	[ ! -d "$1" ] && mv "$1.new.$$" "$1" 2>/dev/null || { rm -f "$1.new.$$"; return 1; }
}

# num VALUE: whether VALUE is a number, digits only. An uptime that could
# not be read is none, and is never taken for 0.
num() {
	case "$1" in ''|*[!0-9]*) return 1 ;; esac
	return 0
}

# key NAME: the stored value of an odi.conf key, or nothing.
key() {
	"$TIMEOUT" "$IO_S" "$FLASH" get "$1" 2>/dev/null | sed -n "s/^$1=//p" | head -n 1
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
		return $?
	fi
	n=$(sed -n '1s/^\([0-9][0-9]*\)$/\1/p' "$COUNTER" 2>/dev/null)
	n=$(( ${n:-0} + 1 ))
	up=$(uptime_s)
	if ! { echo "$n" > "$COUNTER.new.$$" && put "$COUNTER"; }; then
		no_config
		return $?
	fi
	reason=$(prev_reason)
	wdt=$(wdt_update "$reason")
	an=$(key HOST_AUTONEG)
	hs=$(key HOST_SERDES)
	case $hs in 2500basex|hisgmii) ;; *) hs="" ;; esac
	limit=$SAFE_BOOTS
	{ [ "$an" = off ] || [ -n "$hs" ]; } && limit=$SAFE_BOOTS_AN_OFF
	short=$(( n - 1 ))
	event boot_count "boot=$n short_before=$short limit=$limit wdt_before=$wdt wdt_limit=$WDT_BOOTS prev=${reason:-none} uptime=$up"
	if [ "$short" -ge "$limit" ]; then
		enter short_boots "$short" "$wdt" "$an" "$hs" "$reason"
		return $?
	fi
	if [ "$wdt" -ge "$WDT_BOOTS" ]; then
		enter watchdog "$short" "$wdt" "$an" "$hs" "$reason"
		return $?
	fi
	setsid sh "$SELF" boot-ok < /dev/null >> "$LOG" 2>&1 &
	return 0
}

# no_config: the config partition is not mounted or not writable. Nothing
# can be counted, so this boot is a safe-mode boot, with no record (there is
# nowhere to write it; the flag in /var/run still says it).
no_config() {
	say "no writable $CONFIG_DIR: safe mode for this boot" >&2
	enter config 0 0 "" "" "$(prev_reason)"
}

# login_for CAUSE REASON: default when this safe-mode boot opens the default
# credentials, stored when it keeps the saved ones. Only a power cycle opens
# them: a person at the stick, not a reset it did to itself. reason=power is
# a power cycle for every cause, and one of them is the way into a stick
# whose config partition died. Everything the kernel stamped (wdt_*, panic,
# oops, reboot, halt, poweroff) is a restart the stick did itself: stored.
#
# THE ONE OPEN CHOICE is the line marked below: short boots whose last one
# left no stamp at all (unknown), or no ramlog to read (empty). Those are
# default today, because the count is the evidence there and it has to stay
# the way back: a power cut short enough for the DRAM to keep its content
# reads unknown, not power. The price is that an unstamped reset reads the
# same, for one a hard hang that only the hardware watchdog ended, so five
# of those inside the first minute open the default with nobody at the
# stick. Removing that one line makes the rule power only, for every cause
# (test/safe_mode_test.sh, SHORT_UNSTAMPED, flips with it).
login_for() {
	case "$1:$2" in
	*:power) echo default ;;
	short_boots:unknown|short_boots:) echo default ;;	# the one open choice, above
	*) echo stored ;;
	esac
}

# xlock, xunlock: the exit lock, a flock on fd 9 of a file in /var/run that
# is never removed. The timed exit and ack both read the exit= line and
# then rewrite the flag, so without it an ack could be answered ok while
# the reboot it was meant to stop goes ahead. The kernel drops the lock
# when its holder dies, so there is no stale one to take over, and nobody
# ever holds it without having got it. Bounded: flock on the inherited fd
# through timeout, LOCK_S. xlock answers 1 when it did not get the lock,
# and the caller then changes nothing. The probe in a subshell is there
# because a redirection that fails on exec ends the shell.
xlock() {
	( : >> "$EXIT_LOCK" ) 2>/dev/null || return 1
	exec 9>>"$EXIT_LOCK"
	"$TIMEOUT" "$LOCK_S" "$FLOCK" 9 2>/dev/null && return 0
	exec 9>&-
	return 1
}
xunlock() {
	"$FLOCK" -u 9 2>/dev/null
	exec 9>&-
}

# exit_state: the exit= value of the flag (timed, held, exiting, failed).
exit_state() { sed -n 's/^exit=//p' "$FLAG" 2>/dev/null | head -n 1; }

# set_exit VALUE: the flag with its exit= line changed, in one rename.
set_exit() {
	sed "s/^exit=.*\$/exit=$1/" "$FLAG" > "$FLAG.new.$$" 2>/dev/null && put "$FLAG"
}

# A firmware write, by the evidence confd refuses a reboot on (odi-ui
# firmware.c, fwu_starter.sh): the state file says running with a pid that
# is alive (fwu_pid_alive), or the writer lock every process of a write
# inherits is held.
fwu_pid_alive() {
	_st='' _pid=''
	[ ! -f "$FWU_STATE" ] || read -r _st _pid _ < "$FWU_STATE"
	case "$_pid" in ''|*[!0-9]*) _pid='' ;; esac
	[ "$_st" = running ] && [ -n "$_pid" ] && kill -0 "$_pid" 2>/dev/null
}

# wlock: the writer lock itself, taken without waiting, on fd 8, and kept.
# fwu_starter.sh takes the same lock the same way before it touches
# anything and refuses when it is held ("a write is already running"), so
# while the timed exit holds it no write can start: the look for a writer
# and the reboot have no gap between them. 0 held, 1 a writer holds it, 2
# the lock file cannot be made (then nothing is held, and nothing could
# hold it either). Bounded: flock -n through timeout. wunlock lets it go.
wlock() {
	( : >> "$FWU_STATE.lock" ) 2>/dev/null || return 2
	exec 8>>"$FWU_STATE.lock"
	"$TIMEOUT" "$IO_S" "$FLOCK" -n 8 2>/dev/null && return 0
	exec 8>&-
	return 1
}
wunlock() {
	"$FLOCK" -u 8 2>/dev/null
	exec 8>&-
}

# clear_key NAME: remove an odi.conf key, read it back, and add NAME (or
# failed) to the comma-separated list in cleared.
clear_key() {
	"$TIMEOUT" "$IO_S" "$FLASH" set "$1" "" > /dev/null 2>&1
	if [ -z "$(key "$1")" ]; then c=$1; else c=failed; fi
	if [ "$cleared" = none ]; then cleared=$c; else cleared="$cleared,$c"; fi
}

# enter CAUSE SHORT WDT HOST_AUTONEG HOST_SERDES REASON: the safe mode, for
# this boot. CAUSE is short_boots, watchdog or config. HOST_SERDES is the
# stored mode when it is not fiber1g, empty otherwise. REASON is why the
# previous boot ended, which decides the login (login_for). After short boots,
# the host-port keys that lowered the count are removed, so the boots
# after this one stay in Fiber 1G with auto-negotiation too, and so is
# OMCI_UNI_LOCK=1, so an OLT lock that took the host link away cannot do
# it again on the next normal boot.
enter() {
	cause=$1 short=$2 wdt=$3 an=$4 hs=${5:-} reason=${6:-} cleared=none
	login=$(login_for "$cause" "$reason")
	if [ "$cause" = short_boots ]; then
		[ "$an" != off ] || clear_key HOST_AUTONEG
		[ -z "$hs" ] || clear_key HOST_SERDES
		[ "$(key OMCI_UNI_LOCK)" != 1 ] || clear_key OMCI_UNI_LOCK
	fi
	rm -f "$COUNTER" "$WDTFILE"
	# The root password of this boot, after a power cycle only: a copy of
	# the account file in RAM, bind-mounted over the read-only one. Nothing
	# on flash changes. With login=stored the account file is left alone.
	# Before the flag, which says how it went.
	pw=stored
	if [ "$login" = default ]; then
		pw=failed
		if echo "$ROOT_SAFE" > "$PASSWD_RUN" && chmod 644 "$PASSWD_RUN" &&
		   "$TIMEOUT" "$IO_S" "$MOUNT" -o bind "$PASSWD_RUN" "$PASSWD" 2>/dev/null; then
			pw=default
		fi
	fi
	# The flag, whole, in one rename: confd and dropbear read login= from
	# it, the banner exit_at= and exit=. exit_at is the uptime of the timed
	# exit; exit says whether it is due (timed), was acknowledged away
	# (held, by ack), has begun (exiting) or was given up (failed).
	wrote=0
	{
		echo "cause=$cause"
		echo "short_boots=$short"
		echo "wdt_resets=$wdt"
		echo "uptime=$(uptime_s)"
		echo "cleared=$cleared"
		echo "build=$(sed -n 's/^image=//p' "$BUILD" 2>/dev/null | head -n 1)"
		echo "prev=${reason:-none}"
		echo "login=$login"
		echo "root_password=$pw"
		echo "exit_at=$SAFE_EXIT_S"
		echo "exit=timed"
	} > "$FLAG.new.$$" 2>/dev/null && put "$FLAG" && wrote=1
	if [ "$wrote" != 1 ]; then
		# No flag, no safe mode: nothing reads one, and no record says
		# there was one. The default account file must not outlive that;
		# when it cannot be taken away, that is said, not hidden.
		rm -f "$FLAG.new.$$"
		root="root keeps its own account"
		if [ "$pw" = default ]; then
			if "$TIMEOUT" "$IO_S" "$UMOUNT" "$PASSWD" 2>/dev/null; then
				root="the default root account file was taken away again"
			else
				root="the default root account file could NOT be taken away: root answers to the default password until a reboot"
			fi
		fi
		event safe_mode_failed "cause=$cause: could not write $FLAG, this boot is not a safe-mode boot; $root"
		return 1
	fi
	# The record, only now that there is a safe mode to record: the flag
	# without the lines that are about this boot alone.
	if [ "$cause" != config ]; then
		{ sed -e '/^root_password=/d' -e '/^exit_at=/d' -e '/^exit=/d' "$FLAG" > "$RECORD.new.$$" 2>/dev/null && put "$RECORD"; } ||
			say "could not write $RECORD" >&2
	fi
	# What this script did (root), and what it asks of the web UI: login=
	# is a request confd carries out, when it is a confd that reads it.
	case "$login:$pw" in
	default:default) what="login=default: root on the default password admin, the web UI asked to accept admin/admin" ;;
	default:*) what="login=default: the web UI asked to accept admin/admin, root NOT on the default password (the account file could not be replaced: root_password=$pw)" ;;
	*) what="login=stored (no power cycle before it): root and ssh as saved, the web UI asked to keep the saved credential" ;;
	esac
	event safe_mode "cause=$cause short_boots=$short wdt_resets=$wdt cleared=$cleared root_password=$pw prev=${reason:-none} login=$login exit_at=$SAFE_EXIT_S: this boot only, the PON off, the host port in Fiber 1G with auto-negotiation, $what, a reboot at ${SAFE_EXIT_S} s of uptime unless acknowledged"
	setsid sh "$SELF" exit < /dev/null >> "$LOG" 2>&1 &
}

# give_up TEXT: the timed exit ends here with no reboot. The flag stops
# promising one (exit=failed, when the lock and the write allow it), so the
# banner does not go on saying the stick reboots. Answers 1.
give_up() {
	if xlock; then
		[ "$(exit_state)" != timed ] || set_exit failed
		xunlock
	fi
	event safe_mode_exit_failed "$*: no reboot, the safe mode stays up"
	return 1
}

# safe_exit: the end of a safe-mode boot nobody acknowledged. Every wait is
# measured in uptime read again after each sleep, never in the seconds a
# sleep was asked for: one that failed is the end of the exit, one that
# came back early is slept again, and neither is taken for a deadline. An
# uptime that cannot be read ends the exit too; it is never taken for 0.
#   1. The wait to SAFE_EXIT_S of uptime, three sleeps at most.
#   2. Under the exit lock (EXIT_LOCK_TRIES tries, EXIT_POLL_S apart), the
#      flag is read. An ack that got there first holds the boot.
#   3. Still under the lock, a firmware write that is running is not cut.
#      The exit takes the writer lock itself (wlock); when a writer has it,
#      or the state file names a live one, the exit lock is dropped and the
#      reboot waits, looking again every EXIT_POLL_S, until the uptime at
#      which the wait began plus SAFE_EXIT_S. A write takes about eighty
#      seconds, so one still there then is stuck, and the reboot goes ahead
#      and says so.
#   4. The flag goes from exit=timed to exit=exiting. An ack after that is
#      told the reboot has started. So no ack is answered ok and then
#      rebooted over. The writer lock stays held from here through the
#      reboot, so fwu_starter.sh refuses a write that comes now. fwu.sh run
#      by hand takes no lock: acknowledge first (docs/FLASHING.md).
#   5. An orderly reboot (busybox reboot through timeout: init runs its
#      shutdown entries, and the kernel records reason=reboot, which clears
#      the watchdog counter of the next boot). It boots what any reboot
#      boots. One that fails gives the writer lock back and leaves
#      exit=failed and the boot up.
safe_exit() {
	[ -f "$FLAG" ] || return 0
	[ "$EXIT_POLL_S" -ge 1 ] 2>/dev/null || EXIT_POLL_S=1
	tries=0
	while :; do
		up=$(uptime_s)
		if ! num "$up"; then
			give_up "the uptime could not be read ($UPTIME)"
			return 1
		fi
		[ "$up" -lt "$SAFE_EXIT_S" ] || break
		if [ "$tries" -ge 3 ]; then
			give_up "uptime $up s after $tries sleeps, short of ${SAFE_EXIT_S} s"
			return 1
		fi
		if ! "$SLEEP" $(( SAFE_EXIT_S - up )); then
			give_up "the wait for ${SAFE_EXIT_S} s of uptime failed at $up s"
			return 1
		fi
		tries=$((tries + 1))
	done
	grace_from='' gave_up=0 polls=0 lockfails=0 wheld=0
	max_polls=$(( 4 * SAFE_EXIT_S / EXIT_POLL_S + 4 ))
	while :; do
		if ! xlock; then
			lockfails=$((lockfails + 1))
			if [ "$lockfails" -ge "$EXIT_LOCK_TRIES" ]; then
				give_up "the exit lock $EXIT_LOCK was not free in $lockfails tries"
				return 1
			fi
			if ! "$SLEEP" "$EXIT_POLL_S"; then
				give_up "the wait for the exit lock failed"
				return 1
			fi
			continue
		fi
		x=$(exit_state)
		if [ "$x" != timed ]; then
			xunlock
			case "$x" in
			held) say "up $(uptime_s) s: the safe mode was acknowledged, no reboot" ;;
			*) say "up $(uptime_s) s: exit=${x:-none}, not due: no reboot" ;;
			esac
			return 0
		fi
		busy=0
		if fwu_pid_alive; then
			busy=1
		else
			wlock
			case $? in
			0) wheld=1 ;;
			1) busy=1 ;;
			esac
		fi
		if [ "$busy" = 1 ] && [ "$gave_up" = 0 ]; then
			xunlock
			up=$(uptime_s)
			if ! num "$up"; then
				give_up "the uptime could not be read while a firmware write runs ($UPTIME)"
				return 1
			fi
			if [ -z "$grace_from" ]; then
				grace_from=$up
				event safe_mode_exit_wait "state=postponed: a firmware write is running, the reboot waits for it, until uptime $(( grace_from + SAFE_EXIT_S )) s at most"
			fi
			left=$(( grace_from + SAFE_EXIT_S - up ))
			if [ "$left" -le 0 ]; then
				gave_up=1
				event safe_mode_exit_wait "state=gave_up waited=$(( up - grace_from )): a firmware write is still running, rebooting anyway"
				continue
			fi
			polls=$((polls + 1))
			if [ "$polls" -gt "$max_polls" ]; then
				give_up "the wait for the firmware write did not advance in $max_polls sleeps (uptime $up s)"
				return 1
			fi
			[ "$left" -le "$EXIT_POLL_S" ] || left=$EXIT_POLL_S
			if ! "$SLEEP" "$left"; then
				give_up "the wait for the firmware write failed"
				return 1
			fi
			continue
		fi
		if ! set_exit exiting; then
			xunlock
			[ "$wheld" != 1 ] || wunlock
			event safe_mode_exit_failed "could not write $FLAG: no reboot, the safe mode stays up"
			return 1
		fi
		xunlock
		break
	done
	event safe_mode_exit "cause=$(sed -n 's/^cause=//p' "$FLAG" | head -n 1) uptime=$(uptime_s): nobody acknowledged the safe mode, rebooting"
	"$TIMEOUT" "$IO_S" "$REBOOT" && return 0
	[ "$wheld" != 1 ] || wunlock
	if xlock; then
		set_exit failed
		xunlock
	fi
	event safe_mode_exit_failed "reboot failed: the safe mode stays up"
	return 1
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

# ack: someone saw it. A safe-mode boot that was going to end by itself
# stays up instead (exit=held in the flag, which is how a safe mode with no
# record, cause=config, is acknowledged at all), and the record goes. Under
# the exit lock: once the timed exit has claimed the flag (exit=exiting) the
# reboot is on its way, and the answer is that, not ok; the record is kept
# then, so the next boot still says what happened. Without the lock nothing
# is changed and the answer is an error too.
ack() {
	if [ -f "$FLAG" ]; then
		if ! xlock; then
			say "busy: the exit lock $EXIT_LOCK was not free in ${LOCK_S} s. Nothing was acknowledged; try again" >&2
			return 1
		fi
		case "$(exit_state)" in
		exiting)
			xunlock
			say "too late: the timed exit has started and the stick is rebooting. The record is kept" >&2
			return 1
			;;
		timed)
			if ! set_exit held; then
				xunlock
				say "could not write $FLAG: the safe mode still ends at ${SAFE_EXIT_S} s of uptime" >&2
				return 1
			fi
			xunlock
			event safe_mode_hold "the safe mode was acknowledged: it stays up until a reboot"
			;;
		*)
			xunlock
			;;
		esac
	fi
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
exit)    safe_exit ;;
ack)     ack ;;
status)  status ;;
active)  [ -f "$FLAG" ] ;;
*)       echo "usage: $0 count|no-config|boot-ok|exit|ack|status|active" >&2; exit 1 ;;
esac
