#!/bin/sh
# Run an OMCI experiment on a stick, with a restore that survives losing the
# session.
#
# Every phase from the capture onwards takes the OMCI channel from omci_app,
# and the one that programs the switch can cut the very path the session is
# driven over. So the restore must not be the last command of that session. It
# is armed first, from its own connection, and fires on a timer whether or not
# anything else survives.
#
# That is not hypothetical. The first run that touched the switch lost its ssh
# session partway through, the restore never executed, and the stick sat
# without omci_app until it was rebooted by hand.
#
#   testrun.sh <host> <seconds> <command...>
#
# The watchdog is checked before the experiment starts: if arming it fails,
# nothing else runs.
set -u
CTL=${CTL:-/tmp/odi_ctl}
HOST=$1; LIMIT=$2; shift 2

sshq() { ssh -n -S "$CTL" "$HOST" "$@"; }

# omci_app goes in the kill list too. /etc/runomci.sh does not stop a running
# instance, so a restore that fires when nothing was wrong would leave two of
# them fighting over the same queue and the same redirect registration.
RESTORE='killall omcid omcicap omci_app 2>/dev/null; sleep 1; PATH=$PATH:/etc/scripts /etc/runomci.sh'

# Arm first, in a connection that ends immediately, so the timer belongs to no
# session that the experiment can take down. Verified on this device: a
# backgrounded subshell with its stdio detached outlives the connection that
# started it.
sshq "rm -f /tmp/wd.on; ( sleep $LIMIT; $RESTORE; echo done > /tmp/wd.on ) </dev/null >/dev/null 2>&1 & echo armed" \
  | grep -q armed || { echo "could not arm the restore; refusing to run" >&2; exit 1; }
echo "restore armed for ${LIMIT}s"

sshq "$@"
rc=$?

# If we still have the session, restore now rather than waiting out the timer;
# the armed one then finds nothing to do.
sshq "$RESTORE >/dev/null 2>&1; sleep 8; /tmp/omciprobe getOnuState 2>/dev/null | sed -n 2p" || true
exit $rc
