#!/bin/sh
#
# Apply saved settings without a reboot. The web UI runs this after a save
# (odi-ui confd.h APPLY_PATH); it is as usable from a shell.
#
#     apply.sh network   LIVE. Re-apply the management addresses from the
#                        store: LAN_IP_ADDR/LAN_SUBNET (or /etc/config/lan-ip)
#                        and the second address, LAN_ENABLE_IP2, LAN_IP_ADDR2,
#                        LAN_SUBNET2. Prints the plan, then applies it one
#                        second later, in the background -- moving the
#                        primary drops the connection that asked for it, so
#                        the answer has to leave first.
#
#     apply.sh omci      INTERRUPTS INTERNET, for about ten seconds plus
#                        however long the OLT takes to provision again.
#                        Deactivates the ONU, restarts omcid so it reads the
#                        store again, and re-activates: the OLT sees the ONU
#                        range again, resets its MIB and provisions it from
#                        scratch, and the new omcid builds every service with
#                        the settings as they are now. This is what applies
#                        the manual VLAN keys, the LOID, the PLOAM password and
#                        the OLT identity keys.
#
#     apply.sh syslog    SERVICE RESTART. Restarts syslogd so it re-reads
#                        SYSLOG_SERVER. No OMCI interruption, no reboot: the
#                        respawn entry (svc-syslogd.sh) checks the store on
#                        every start the same way every other svc-*.sh does,
#                        so killing the running one is the whole apply.
#
#     apply.sh ntp       SERVICE RESTART. Restarts ntpd so it re-reads
#                        NTP_SERVER -- starting it for the first time if the
#                        key was just set, or stopping it if just cleared
#                        (svc-ntpd.sh's own off-flag-shaped gate on the key
#                        being present at all).
#
# Why omci needs the re-activation. omcid reads the store once, at start, and
# builds connections only when the OLT provisions them. A restart alone gives
# a daemon with the new settings and an empty MIB, while the switch keeps the
# old connections, until the OLT next audits the MIB -- which may be never.
# gpondeact and gponact are the kernel verbs rcS itself uses; the sequence
# (deactivate, three seconds, activate) is the forced re-activation measured
# on hardware: O1 to O5 in about five seconds, OMCI resumed by itself.
#
# The serial number is not re-applied: the kernel writes it into the PON MAC
# once, at the first activation of a boot. A new GPON_SN needs a reboot.
set -u

# write_proc_bounded: the bounded /proc-verb write rcS and rcS.pon already
# use, reused here rather than a second copy -- see rcs-lib.sh for what
# `timeout` does and does not catch on these. Relative to the directory
# this script lives in, not a fixed /etc/scripts/ path, so
# test/apply_test.sh (which runs this file straight out of the working
# tree) finds the real one beside it instead of a device path that does
# not exist off the stick.
RCS_LIB=${RCS_LIB:-$(dirname "$0")/rcs-lib.sh}
# shellcheck source=./rcs-lib.sh
. "$RCS_LIB"

ODI_INIT=${ODI_INIT:-/proc/odi_init}
ODI_OMCI=${ODI_OMCI:-/proc/odi_omci}
OMCID=${OMCID:-/bin/omcid}
OMCID_LOG=${OMCID_LOG:-/var/log/omcid.log}
NETWORK=${NETWORK:-/etc/scripts/network.sh}
CS=${CS:-/etc/config/lastgood.xml}
MODULES_OFF=${MODULES_OFF:-/etc/config/modules.off}
SERVICES_LOG=${SERVICES_LOG:-/var/log/services.log}

# How long each wait may take, in seconds.
STOP_WAIT=${STOP_WAIT:-10}          # omcid deregisters and exits on SIGTERM; slack
REGISTER_WAIT=${REGISTER_WAIT:-10}  # the new omcid registers for redirect type 1
DEACT_HOLD=${DEACT_HOLD:-3}         # deactivated at least this long, as measured

say() { echo "apply: $*"; }
die() { echo "apply: $*" >&2; exit 1; }

network() {
	[ -x "$NETWORK" ] || die "no $NETWORK"
	# The plan, synchronously, so the caller sees what will change.
	sh "$NETWORK" addr -n || die "network.sh could not work out the addresses"
	setsid sh -c 'sleep 1; exec sh "$1" addr' apply "$NETWORK" \
		< /dev/null >> "$SERVICES_LOG" 2>&1 &
	say "applying in one second"
}

omci() {
	[ ! -f "$MODULES_OFF" ] || die "$MODULES_OFF is set: this boot runs no omcid, nothing to restart"
	[ -w "$ODI_INIT" ] || die "no $ODI_INIT: this kernel has no PON verbs"
	[ -x "$OMCID" ] || die "no $OMCID"

	t0=$(uptime_s)
	write_proc_bounded "$ODI_INIT" gpondeact 2>/dev/null || die "gpondeact failed; nothing else was touched"
	say "ONU deactivated (internet is down from here)"

	old=$(pidof omcid)
	if [ -n "$old" ]; then
		# shellcheck disable=SC2086 # one pid per word
		kill $old 2>/dev/null
		i=0
		while [ "$i" -lt "$STOP_WAIT" ] && pidof omcid > /dev/null 2>&1; do
			sleep 1
			i=$((i + 1))
		done
		if pidof omcid > /dev/null 2>&1; then
			# A process that will not deregister is worse than one that
			# was killed: a new omcid refuses to start while a live one
			# holds redirect type 1.
			say "omcid $old did not stop on SIGTERM, killing it"
			# shellcheck disable=SC2086
			kill -9 $old 2>/dev/null
			sleep 1
		fi
		say "omcid $old stopped"
	fi

	# Its own session and no terminal, so no SIGHUP can reach it: omcid
	# treats SIGHUP as stop. -r clears the bridge connections the old one
	# left in the switch before this one builds its own.
	setsid "$OMCID" -a -d -r >> "$OMCID_LOG" 2>&1 < /dev/null &

	# The wait for registration, the PLOAM password re-apply, the hold and
	# the final gponact are the tail svc-omcid.sh's automatic
	# re-provisioning (docs/BOOT.md) also runs, after a respawn -- shared
	# as rcs-lib.sh's omci_reactivate rather than kept as a second copy.
	new=$(omci_reactivate "$t0" "$REGISTER_WAIT" "$DEACT_HOLD") \
		|| die "gponact FAILED: the ONU stays off the line until a reboot"
	if [ -n "$new" ]; then
		say "omcid $new registered for OMCI"
	else
		say "omcid did not register within ${REGISTER_WAIT} s -- activating anyway, see $OMCID_LOG" >&2
	fi
	say "ONU re-activated; the OLT provisions it again within about a minute"
	[ -n "$new" ] || exit 1
}

# restart_respawn <name>: kill whatever is running under this daemon's
# respawn entry (respawn.sh execs the daemon itself in place, so its pid IS
# what pidof finds -- same as omci() above using `pidof omcid`) and let
# init's own respawn bring it back, re-reading the store: every svc-*.sh
# checks it fresh on every start, off flag and setting both. Also the way a
# daemon that was off (or, for ntpd, unconfigured) starts for the first
# time: respawn-off.sh's own pid does not match $name, so this just reports
# nothing was running, and the next scheduled respawn of that placeholder
# (it loops in one-hour sleeps, so this does not START it sooner -- restart
# the respawn-off.sh pid itself, found the same way, when the one-hour wait
# would otherwise delay it) is what picks up the change. To take effect at
# once either way, this restarts whichever of the two is currently running.
restart_respawn() {
	name=$1
	pid=$(pidof "$name" 2>/dev/null | head -n 1)
	if [ -z "$pid" ]; then
		pid=$(pgrep -f "/etc/scripts/respawn-off.sh" 2>/dev/null | head -n 1)
	fi
	if [ -z "$pid" ]; then
		say "$name: no running process found, nothing to restart"
		return 0
	fi
	kill "$pid" 2>/dev/null
	say "$name (pid $pid) restarted; init will bring it back with the new setting"
}

syslog() {
	restart_respawn syslogd
}

ntp() {
	restart_respawn ntpd
}

case "${1:-}" in
network) network ;;
omci)    omci ;;
syslog)  syslog ;;
ntp)     ntp ;;
*)       echo "usage: $0 network|omci|syslog|ntp" >&2; exit 1 ;;
esac
exit 0
