#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# slot-state.sh -- record, at boot, which slot is running and whether it is
# committed, and say so where it will be seen (docs/FLASHING.md,
# "Committing"). Run by /etc/inittab as a `once` entry; also safe to run by
# hand at any time, which is how the notice is refreshed after `nv commit`:
# the web UI (confd, which runs its children with an EMPTY environment) runs
# it after a commit, so it sets its own PATH and does not wait for syslogd
# on a run after the first of a boot.
#
# An image of ours is UNCOMMITTED when sw_commit, in either copy of the
# U-Boot environment, is not the running slot: the next reset (or, for the
# fallback copy, the next time the primary copy is lost) boots another slot.
# That is exactly the state a trial is in, and it is meant to end with a
# deliberate `nv commit <slot>` once the trial has been checked -- so this
# makes the state impossible to miss rather than acting on it:
#
#   $STATE (/var/run/odi-slot)   KEY=value, for the exporter and the web UI;
#                                the format is documented in docs/TOOLS.md
#   $MOTD  (/var/run/motd)       what dropbear prints at an interactive
#                                login (/etc/motd is a link to it)
#   syslog                       one line, tag slot-state, when not committed
#
# It never writes the environment. Both files are written to a temporary
# name and renamed, so a reader never sees half of one. /var is tmpfs: the
# state is rebuilt on every boot, never carried over.
#
# It also checks the OTHER slot, the one a reverted trial boots and the only
# way back after a commit: `nv slotcheck`, a PARTIAL check (src/nv/slot.h)
# that the kernel there still has a valid uImage header, data CRC and size
# and the rootfs starts with a squashfs superblock. A slot that fails it
# would not boot as it is, and the banner says so; one that passes is not
# proven to boot. The check reads up to 1.3 MB of flash, so it runs AFTER
# the state file is first written: trial-guard.sh waits only STATE_WAIT_S
# for that file, and must not wait on the read. The file is then written
# again with the result, still by rename.
#
# The running slot is read the way fwu.sh and nv read it: root=31:N in
# /proc/cmdline, N the index of the MTD partition named r0 or r1.
set -u

# confd starts its children with no environment at all. Make the lookups of
# timeout, sed, head, sleep and logger independent of whoever calls this.
PATH=${SLOT_STATE_PATH:-/sbin:/usr/sbin:/bin:/usr/bin}${PATH:+:$PATH}
export PATH

NV=${NV:-/bin/nv}
LOGGER=${LOGGER:-logger}
PROC_CMDLINE=${PROC_CMDLINE:-/proc/cmdline}
PROC_MTD=${PROC_MTD:-/proc/mtd}
STATE=${STATE:-/var/run/odi-slot}
MOTD=${MOTD:-/var/run/motd}
DEV_LOG=${DEV_LOG:-/dev/log}
RCS_DEV=${RCS_DEV:-/etc/init.d/rcS.dev}
BREADCRUMBS_ON=${BREADCRUMBS_ON:-/etc/config/breadcrumbs.on}
CONFIRM_ARP_FILE=${CONFIRM_ARP_FILE:-/etc/config/confirm-arp}
# nv reads two 8 KB partitions; 10 s is generous.
NV_TIMEOUT_S=${NV_TIMEOUT_S:-10}
# nv slotcheck reads at most the 1.3 MB kernel partition and 96 bytes of the
# rootfs one; NOR reads at well over 1 MB/s, so 30 s is a stuck read.
SLOTCHECK_TIMEOUT_S=${SLOTCHECK_TIMEOUT_S:-30}
LOG_WAIT_S=${LOG_WAIT_S:-30}

running_slot() {
	root=$(sed -n 's/.*root=31:\([0-9]*\).*/\1/p' "$PROC_CMDLINE" 2>/dev/null)
	[ -n "$root" ] || return 0
	r0=$(sed -n 's/^mtd\([0-9]*\): .*"r0"$/\1/p' "$PROC_MTD" 2>/dev/null)
	r1=$(sed -n 's/^mtd\([0-9]*\): .*"r1"$/\1/p' "$PROC_MTD" 2>/dev/null)
	[ "$root" = "$r0" ] && echo 0
	[ "$root" = "$r1" ] && echo 1
	return 0
}

# One value out of an nv dump.
kv() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -n 1; }
slot_or_empty() { case "$1" in 0 | 1) echo "$1" ;; esac; }

running=$(running_slot)
# The whole environment in one read each: nv getenv with no name prints
# "Valid environment: N" (the copy U-Boot boots from) and every pair; nv
# fallback the same for the other copy, or nothing when it is not valid.
primary_dump=$(timeout "$NV_TIMEOUT_S" "$NV" getenv 2>/dev/null)
fallback_dump=$(timeout "$NV_TIMEOUT_S" "$NV" fallback 2>/dev/null)
primary_copy=$(printf '%s\n' "$primary_dump" | sed -n 's/^Valid environment: \([12]\)$/\1/p')
fallback_copy=$(printf '%s\n' "$fallback_dump" | sed -n 's/^Fallback environment: \([12]\)$/\1/p')
active=$(slot_or_empty "$(kv "$primary_dump" sw_active)")
tryactive=$(kv "$primary_dump" sw_tryactive)
commit_p=$(slot_or_empty "$(kv "$primary_dump" sw_commit)")
commit_f=
[ -n "$fallback_copy" ] && commit_f=$(slot_or_empty "$(kv "$fallback_dump" sw_commit)")

# The slot the next reset boots: a pending trial wins, else the primary
# copy of sw_commit (what boot_by_commit reads).
next=$(slot_or_empty "$tryactive")
[ -n "$next" ] || next=$commit_p

# uncommitted: 1 when either valid copy names another slot (or none), 0
# when every valid copy names the running slot, empty when that cannot be
# told -- no running slot, or no readable environment at all.
uncommitted=
if [ -n "$running" ] && [ -n "$primary_copy" ]; then
	uncommitted=0
	[ "$commit_p" = "$running" ] || uncommitted=1
	[ -z "$fallback_copy" ] || [ "$commit_f" = "$running" ] || uncommitted=1
fi

# The notice. Empty when committed: nothing to say.
if [ -z "$running" ]; then
	notice="SLOT STATE UNKNOWN: no root=31:N naming r0 or r1 in $PROC_CMDLINE, so the running slot cannot be told. Check with: nv getenv; nv fallback"
elif [ -z "$primary_copy" ]; then
	notice="SLOT STATE UNKNOWN: running slot $running, but nv could not read a valid U-Boot environment. Check with: nv getenv; nv fallback"
elif [ "$uncommitted" = 0 ]; then
	notice=
elif [ "$commit_p" != "$running" ] && { [ -z "$fallback_copy" ] || [ "$commit_f" != "$running" ]; }; then
	if [ -z "$fallback_copy" ]; then
		where="sw_commit=${commit_p:-unset}, and there is no valid fallback copy"
	elif [ "$commit_f" = "$commit_p" ]; then
		where="sw_commit=${commit_p:-unset} in both copies"
	else
		where="sw_commit=${commit_p:-unset} in the primary copy ($primary_copy), ${commit_f:-unset} in the fallback copy ($fallback_copy)"
	fi
	notice="TRIAL BOOT: running slot $running, which is not committed ($where). The next reboot returns to slot ${next:-unknown}. Commit with: nv commit $running && slot-state.sh"
elif [ "$commit_p" != "$running" ]; then
	notice="TRIAL BOOT: running slot $running is committed only in the fallback copy ($fallback_copy); the primary copy ($primary_copy) says sw_commit=${commit_p:-unset}. The next reboot returns to slot ${next:-unknown}. Commit with: nv commit $running && slot-state.sh"
else
	notice="HALF COMMITTED: running slot $running is committed in the primary copy ($primary_copy), but the fallback copy ($fallback_copy) still says sw_commit=${commit_f:-unset}; if the primary copy is ever lost, U-Boot boots that instead. Fix with: nv commit $running && slot-state.sh"
fi

# The state file is rebuilt on every boot (/var is tmpfs), so its presence
# means an earlier run this boot: the syslogd race below is a boot-time
# problem only, and a refresh after a commit must not hang on it.
first_run=1
[ -e "$STATE" ] && first_run=0

# Temp file beside the target, then rename: a reader never sees half of it.
put() {
	mkdir -p "$(dirname "$1")" 2>/dev/null
	if printf '%s' "$2" > "$1.tmp.$$" && mv "$1.tmp.$$" "$1"; then
		return 0
	fi
	rm -f "$1.tmp.$$"
	echo "slot-state: could not write $1" >&2
	return 1
}

# The other slot: only when the running one is known, since that is what
# "other" is measured from. Until the check below has answered, the file
# carries what the last run this boot found for the same slot, or pending:
# a refresh cut short (confd gives this script 12 s) must not wipe an
# OTHER SLOT BROKEN it cannot replace.
other=
other_check=
other_detail=
other_notice=
case $running in
0 | 1)
	other=$((1 - running))
	other_check=pending
	if [ "$first_run" = 0 ] && [ "$(sed -n 's/^other_slot=//p' "$STATE" 2>/dev/null | head -n 1)" = "$other" ]; then
		prev=$(sed -n 's/^other_slot_check=//p' "$STATE" 2>/dev/null | head -n 1)
		case $prev in
		ok | fail | unknown)
			other_check=$prev
			other_detail=$(sed -n 's/^other_slot_detail=//p' "$STATE" 2>/dev/null | head -n 1) ;;
		esac
	fi ;;
esac

# The banner line for other_check and other_detail.
set_other_notice() {
	other_notice=
	case $other_check in
	fail)
		# On a trial the other slot is where the next reboot goes; on a
		# committed slot it is the only way back, through a trial armed
		# by hand.
		if [ "$next" = "$other" ]; then
			what="and the next reboot boots it: until it is flashed again or slot $running is committed, a reboot leaves the stick on a slot that does not boot."
		else
			what="and there is no working slot to go back to. Flash it again (fwu.sh) to have one."
		fi
		other_notice="OTHER SLOT BROKEN: slot $other fails a partial check ($other_detail), so it would not boot as it is, $what Check again with: nv slotcheck $other && slot-state.sh" ;;
	unknown)
		other_notice="OTHER SLOT UNCHECKED: slot $other could not be checked ($other_detail). Check with: nv slotcheck $other" ;;
	esac
}
set_other_notice

write_state() {
	put "$STATE" "running=$running
sw_active=$active
sw_tryactive=$tryactive
primary_copy=$primary_copy
primary_sw_commit=$commit_p
fallback_copy=$fallback_copy
fallback_sw_commit=$commit_f
next_boot=$next
uncommitted=$uncommitted
other_slot=$other
other_slot_check=$other_check
other_slot_detail=$other_detail
"
}

# The development toggles, on a committed slot too: confirm-arp turns an
# unreachable host into a reset every 120 s, and neither is meant to stay.
# The address is read by the code that rcS.dev boots with, so the banner and
# the boot agree on what counts as a valid one.
aids=
if [ -f "$CONFIRM_ARP_FILE" ]; then
	GW=
	# shellcheck disable=SC1090
	[ -r "$RCS_DEV" ] && . "$RCS_DEV"
	command -v dev_gw >/dev/null 2>&1 && dev_gw
	if [ -n "$GW" ]; then
		aids="*** DEV AID confirm-arp: $CONFIRM_ARP_FILE is present, so every boot waits for an ARP reply from $GW before confirming the watchdog; if that host stops answering, the stick resets every 120 s. Remove it when the trial is done.
"
	else
		aids="*** DEV AID confirm-arp: $CONFIRM_ARP_FILE is present with no address: has no effect on the boot. nv commit still refuses while it exists; remove it.
"
	fi
fi
if [ -f "$BREADCRUMBS_ON" ]; then
	aids="$aids*** DEV AID breadcrumbs.on: $BREADCRUMBS_ON is present (boot breadcrumbs and extra logging); remove it when you are not debugging.
"
fi
write_motd() {
	if [ -n "$notice" ] || [ -n "$other_notice" ] || [ -n "$aids" ]; then
		text="
"
		[ -z "$notice" ] || text="$text*** $notice
"
		[ -z "$other_notice" ] || text="$text*** $other_notice
"
		put "$MOTD" "$text$aids
"
	else
		put "$MOTD" ""
	fi
}

write_state
write_motd

# Now the slow part. nv prints one line: "... passes the partial check ...",
# "... FAILS the partial check: <reason>" (exit 1) or "... not checked:
# <reason>" (exit 2). Anything else -- a timeout, an nv without the verb --
# is an unknown, never a pass and never a failure.
if [ -n "$other" ]; then
	out=$(timeout "$SLOTCHECK_TIMEOUT_S" "$NV" slotcheck "$other" 2>/dev/null)
	rc=$?
	line=$(printf '%s\n' "$out" | head -n 1)
	case "$rc:$line" in
	"0:"*"passes the partial check"*)
		other_check=ok
		other_detail=${line#nv: } ;;
	"1:"*"FAILS the partial check: "*)
		other_check=fail
		other_detail=${line#*FAILS the partial check: } ;;
	"2:"*"not checked: "*)
		other_check=unknown
		other_detail=${line#*not checked: } ;;
	*)
		other_check=unknown
		other_detail="nv slotcheck gave no answer (exit $rc)" ;;
	esac
	set_other_notice
	write_state
	write_motd
fi

[ -n "$notice" ] || [ "$other_check" = fail ] || exit 0
# At boot this runs in the same breath as the syslogd respawn entry, and a
# line logged before syslogd owns /dev/log is lost. Bounded: with syslogd
# off the socket never appears and the line is dropped after LOG_WAIT_S. Only
# on the first run of a boot; a refresh logs at once or not at all.
i=0
[ "$first_run" = 1 ] || i=$LOG_WAIT_S
while [ ! -e "$DEV_LOG" ] && [ "$i" -lt "$LOG_WAIT_S" ]; do
	sleep 1
	i=$((i + 1))
done
[ -z "$notice" ] || timeout 5 "$LOGGER" -t slot-state -p daemon.warning "$notice" 2>/dev/null || true
[ "$other_check" != fail ] || timeout 5 "$LOGGER" -t slot-state -p daemon.warning "$other_notice" 2>/dev/null || true
exit 0
