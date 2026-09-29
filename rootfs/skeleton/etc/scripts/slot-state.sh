#!/bin/sh
# slot-state.sh -- record, at boot, which slot is running and whether it is
# committed, and say so where it will be seen (docs/FLASHING.md,
# "Committing"). Run by /etc/inittab as a `once` entry; also safe to run by
# hand at any time, which is how the notice is refreshed after `nv commit`.
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
# The running slot is read the way fwu.sh and nv read it: root=31:N in
# /proc/cmdline, N the index of the MTD partition named r0 or r1.
set -u

NV=${NV:-/bin/nv}
LOGGER=${LOGGER:-logger}
PROC_CMDLINE=${PROC_CMDLINE:-/proc/cmdline}
PROC_MTD=${PROC_MTD:-/proc/mtd}
STATE=${STATE:-/var/run/odi-slot}
MOTD=${MOTD:-/var/run/motd}
DEV_LOG=${DEV_LOG:-/dev/log}
# nv reads two 8 KB partitions; 10 s is generous.
NV_TIMEOUT_S=${NV_TIMEOUT_S:-10}
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

put "$STATE" "running=$running
sw_active=$active
sw_tryactive=$tryactive
primary_copy=$primary_copy
primary_sw_commit=$commit_p
fallback_copy=$fallback_copy
fallback_sw_commit=$commit_f
next_boot=$next
uncommitted=$uncommitted
"
if [ -n "$notice" ]; then
	put "$MOTD" "
*** $notice

"
else
	put "$MOTD" ""
fi

[ -n "$notice" ] || exit 0
# At boot this runs in the same breath as the syslogd respawn entry, and a
# line logged before syslogd owns /dev/log is lost. Bounded: with syslogd
# off the socket never appears and the line is dropped after LOG_WAIT_S.
i=0
while [ ! -e "$DEV_LOG" ] && [ "$i" -lt "$LOG_WAIT_S" ]; do
	sleep 1
	i=$((i + 1))
done
timeout 5 "$LOGGER" -t slot-state -p daemon.warning "$notice" 2>/dev/null || true
exit 0
