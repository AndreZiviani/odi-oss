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
#     apply.sh omci [SECONDS]
#                        Tells the running omcid to reread the config store
#                        (SIGHUP, the Unix convention for a reload) and waits,
#                        bounded, for the outcome. omcid compares the store
#                        with what it is running and does the least that
#                        applies:
#                          - nothing changed: says so, touches nothing;
#                          - only the VLAN keys (VLAN_CFG_TYPE, VLAN_MANU_MODE,
#                            VLAN_MANU_TAG_VID, VLAN_MANU_TAG_PRI): rebuilds
#                            the bridge connections in place from the MIB it
#                            holds. LIVE: the fibre stays in O5, nothing is
#                            re-ranged;
#                          - anything the OLT has to see again (GPON_SN, the
#                            PLOAM password, the LOID keys, the identity keys
#                            and switch, OMCI_UNKNOWN_ME_OK): omcid
#                            deactivates the ONU, clears its MIB, hands the
#                            driver the new serial number and password and
#                            activates again, and the OLT provisions it from
#                            scratch. INTERRUPTS INTERNET for about ten
#                            seconds plus the OLT provisioning time. The
#                            process is never restarted.
#                        It waits SECONDS (default 20, APPLY_WAIT; the web UI
#                        gives this script 25) for the outcome: reload done,
#                        or for a re-registration back in O5 with services,
#                        and reports how long that took. A re-registration
#                        still running at the bound is reported as running and
#                        is not a failure; omcid carries on and finishes it.
#                        Exit 1 when no omcid runs, when omcid does not answer
#                        the signal, or when the reload failed.
#                        It never starts an omcid: init owns the process
#                        (inittab respawns svc-omcid.sh), and a second one
#                        made the ONU cycle through O1 to O3 on hardware.
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
# Why a signal and not a restart. omcid reads the store at start and builds
# connections only when the OLT provisions them, so a changed store needs
# either a new process with an empty MIB (which the OLT then has to be forced
# to reprovision, and which init would respawn on top of) or the running
# process rereading it. The second is what SIGHUP has always meant, and the
# only one that leaves a VLAN-only change without a re-ranging. omcid does the
# gpondeact/gponact steps itself when a re-registration is needed
# (src/omci/respond/reload.c, docs/SETTINGS.md "How apply works").
#
# What omcid tells this script is in /var/run/omcid-reload, one key=value per
# line, replaced whole by a rename: id (pid.sequence), state (running or
# done), changed, action, result, keys (names, never values), duration_ms,
# o5_ms, services.
#
set -u

RELOAD_STATUS=${RELOAD_STATUS:-/var/run/omcid-reload}
NETWORK=${NETWORK:-/etc/scripts/network.sh}
MODULES_OFF=${MODULES_OFF:-/etc/config/modules.off}
SERVICES_LOG=${SERVICES_LOG:-/var/log/services.log}

# How long each wait may take, in seconds.
ACK_WAIT=${ACK_WAIT:-5}             # omcid takes the signal and starts the reload
APPLY_WAIT=${APPLY_WAIT:-20}        # the outcome; under the web UI 25 s bound

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

# status_get <key>: one value of the status file, empty when absent.
status_get() { sed -n "s/^$1=//p" "$RELOAD_STATUS" 2>/dev/null | head -n 1; }

# secs <milliseconds>: whole and tenths of a second, for the report.
secs() { echo "$(( ${1:-0} / 1000 )).$(( ${1:-0} % 1000 / 100 ))"; }

omci() {
	wait_s=${1:-$APPLY_WAIT}
	[ ! -f "$MODULES_OFF" ] || die "$MODULES_OFF is set: this boot runs no omcid, nothing to reload"
	pids=$(pidof omcid 2>/dev/null)
	[ -n "$pids" ] || die "no omcid is running: nothing to reload (init respawns it; see /var/log/omcid.log)"

	before=$(status_get id)
	# shellcheck disable=SC2086 # one pid per word
	kill -HUP $pids 2>/dev/null || die "could not signal omcid $pids"
	say "omcid $pids told to reload its configuration"

	# The acknowledgement: a new run appears in the status file. A daemon
	# that never writes one is an older build, where SIGHUP means stop.
	i=0
	while :; do
		id=$(status_get id)
		[ -n "$id" ] && [ "$id" != "$before" ] && break
		[ "$i" -lt "$ACK_WAIT" ] || die "omcid did not answer the signal within ${ACK_WAIT} s (an older build treats SIGHUP as stop; init respawns it)"
		sleep 1
		i=$((i + 1))
	done

	# The outcome, bounded. Counted in iterations of one second: this is
	# also what runs under the test fixture, where uptime is a constant.
	i=0
	while [ "$(status_get state)" != "done" ] && [ "$i" -lt "$wait_s" ]; do
		sleep 1
		i=$((i + 1))
	done

	changed=$(status_get changed) result=$(status_get result)
	keys=$(status_get keys)
	if [ "$(status_get state)" != "done" ]; then
		say "$changed: still working after ${wait_s} s (${result}); omcid carries on and logs event=reload_done when it ends"
		return 0
	fi
	dur=$(secs "$(status_get duration_ms)")
	case "$changed:$result" in
	none:ok)
		say "nothing changed in the store; nothing done" ;;
	vlan:ok)
		say "VLAN handling changed ($keys): connections rebuilt in place in ${dur} s, the ONU stayed in O5" ;;
	identity:ok)
		say "identity changed ($keys): the ONU re-registered and is back in O5 with $(status_get services) service(s) after $(secs "$(status_get o5_ms)") s (omcid $(status_get duration_ms) ms in all)" ;;
	*)
		say "$changed ($keys): $result after ${dur} s -- see /var/log/omcid.log (event=reload_done)" >&2
		case $result in
		failed)         say "the driver refused a step; the ONU may be off the line until the next reload or a reboot" >&2 ;;
		timeout)        say "the ONU did not reach O5 in time; the OLT may not know the new identity" >&2 ;;
		no_services)    say "the ONU is in O5 but the OLT built no services" >&2 ;;
		sn_not_applied) say "the ONU is back, but the driver did not take the new serial number" >&2 ;;
		esac
		return 1 ;;
	esac
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
omci)    shift; omci "$@" || exit 1 ;;
syslog)  syslog ;;
ntp)     ntp ;;
*)       echo "usage: $0 network|omci [seconds]|syslog|ntp" >&2; exit 1 ;;
esac
exit 0
