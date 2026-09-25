#!/bin/sh
# One OMCI experiment, run entirely on the stick.
#
# The session that starts an experiment cannot be relied on to see it through:
# the path to this device flaps, and the experiment itself can disturb
# forwarding. So nothing here depends on a connection staying up. The caller
# copies this over, starts it detached, and comes back for the log.
#
#   ontest.sh <seconds> [args for omcid...]
#
# It always ends by putting omci_app back, whatever happened in between --
# including if omcid hung, since the kill is on a timer rather than on omcid
# finishing.
#
# It also samples the switch counters around the experiment. Those are the only
# thing on this device that says whether it is forwarding: O5 with every alarm
# clear says nothing, /proc/net/dev sees none of it, and the mirror between the
# PON port and the host port is the whole signal. Four samples -- two before
# omci_app is stopped, two after our own stack has provisioned -- turn that into
# a rate that can be compared rather than a number that can be misread.
set -u
SECS=${1:-90}; shift 2>/dev/null || true
LOG=/tmp/ontest.log
SETTLE=${SETTLE:-30}

exec > "$LOG" 2>&1
echo "== start $(date) : omcid $* for ${SECS}s"

# `all` here means every port that answers, which on this device is 0, 2 and 3;
# the vendor's own port set omits 3. Port 2 is the PON side and port 0 the host
# side.
snap() {
	echo "-- counters $1"
	/tmp/diag 'mib dump counter port all' 2>/dev/null |
	  grep -E '^Port:|ifInOctets|ifOutOctets|ifInUcastPkts|ifOutUcastPkts'
}

snap "t0 (oem)"
sleep "$SETTLE"
snap "t1 (oem)"

/tmp/omcid "$@" -w 12 > /tmp/omcid.log 2>&1 &
OMCID=$!
sleep 2

killall omci_app 2>/dev/null
sleep 2

# The pair OMCI_Init ends with; this is what makes the OLT re-register us, and
# therefore what makes it talk at all.
/tmp/omciprobe -f activateGpon 00 00 00 00 >/dev/null 2>&1
sleep 2
/tmp/omciprobe -f activateGpon 00 00 00 01 >/dev/null 2>&1

# Give the OLT its provisioning run before the first sample, or the window
# measures registration rather than forwarding.
sleep "$SETTLE"
snap "t2 (ours)"
sleep "$SECS"
snap "t3 (ours)"

# SIGTERM now reaches a handler that gives the redirect registration back.
# Leaving it stale is what made the kernel printk once per undeliverable frame,
# and the OLT generates those in bursts; close the gap rather than widen it.
kill $OMCID 2>/dev/null
sleep 2
killall omcid omcicap omci_app 2>/dev/null
sleep 1
PATH=$PATH:/etc/scripts /etc/runomci.sh >/dev/null 2>&1
sleep 8

echo "== gave the registration back: $(grep -c 'deregistered on signal\|done after' /tmp/omcid.log)"
echo "== frames: $(grep -c '^<-' /tmp/omcid.log)"
echo "== capabilities:"
grep -E 'capabilities:|uni slot' /tmp/omcid.log
echo "== hardware calls:"
grep 'hw\]' /tmp/omcid.log
echo "== onu state:"
/tmp/omciprobe getOnuState | sed -n 2p
echo "== omci_app threads: $(ps | grep -c '[o]mci_app')"
echo "== done $(date)"
