#!/bin/sh
# S2: does OUR 160-byte bridge connection descriptor reach the driver and
# restore forwarding after the vendor's connections are torn down?
#
# The question this settles is the one `activeBdgConn is load-bearing` left
# open. That experiment proved the vendor's connections are what forward --
# tear them down and upstream stops dead with every GEM flow still in place.
# It did NOT show that ours can take their place, because ours had never been
# sent. This sends one.
#
# Five windows, each `WIN` seconds of switch counters:
#
#   w0  oem            omci_app running, vendor connections up
#   w1  ours, theirs   our omcid provisioned, vendor connections STILL in the
#                      driver -- stopping omci_app does not clear them
#   w2  torn down      both vendor connections removed by index
#   w3  ours           our own activeBdgConn sent for the same ingress and GEM
#   w4  restored       omci_app back
#
# w2 is the control that makes w3 mean anything: without seeing forwarding
# actually stop, "it forwarded" says nothing.
#
# Downstream is the signal, not upstream. The host's PPPoE drops as soon as the
# service connection goes, so there is no upstream traffic left to forward --
# but the OLT keeps sending downstream regardless, and this ONU floods each
# frame two to three times. Port 3 is the control: it is the CPU port's own
# management traffic and must keep moving throughout, or the sample is measuring
# a dead stick rather than a dead connection.
#
# Everything is restored on the way out whatever happened in between, including
# if omcid hung -- the kill is on a timer, not on omcid finishing.
set -u
WIN=${WIN:-30}
SETTLE=${SETTLE:-30}
INGRESS=${INGRESS:-0x601}     # isp2's VEIP; isp1 would be a PPTP Ethernet UNI
GEM=${GEM:-657}
DIR=${DIR:-3}
NCONN=${NCONN:-2}             # vendor connections to tear down, 0..NCONN-1
LOG=/tmp/bdgtest.log

exec > "$LOG" 2>&1
echo "== start $(date) : ingress $INGRESS gem $GEM dir $DIR, ${WIN}s windows"

snap() {
	echo "-- counters $1"
	/tmp/diag 'mib dump counter port all' 2>/dev/null |
	  grep -E '^Port:|ifInOctets|ifOutOctets'
}

win() {                       # win <label>
	snap "$1 begin"
	sleep "$WIN"
	snap "$1 end"
}

win "w0 oem"

/tmp/omcid -a -w 12 > /tmp/omcid.log 2>&1 &
OMCID=$!
sleep 2
killall omci_app 2>/dev/null
sleep 2
/tmp/omciprobe -f activateGpon 00 00 00 00 >/dev/null 2>&1
sleep 2
/tmp/omciprobe -f activateGpon 00 00 00 01 >/dev/null 2>&1
sleep "$SETTLE"

win "w1 ours-with-their-conns"

echo "-- tearing down $NCONN vendor connections"
i=0
while [ "$i" -lt "$NCONN" ]; do
	echo "   deactiveBdgConn $i:"
	/tmp/omciprobe -f deactiveBdgConn 00 00 00 "0$i" 2>&1
	i=$((i + 1))
done
sleep 3

win "w2 torn-down"

echo "-- our own activeBdgConn:"
/tmp/omcli bridge "$INGRESS" "$GEM" "$DIR" 2>&1
sleep 3

win "w3 ours"

echo "-- restoring"
kill $OMCID 2>/dev/null
sleep 2
killall omcid omcicap omci_app 2>/dev/null
sleep 1
PATH=$PATH:/etc/scripts /etc/runomci.sh >/dev/null 2>&1
sleep 20

win "w4 restored"

echo "== omcid hardware calls:"
grep 'hw\]' /tmp/omcid.log
echo "== omcid bridge lines:"
grep -iE 'bdgconn|serv ' /tmp/omcid.log
echo "== onu state:"
/tmp/omciprobe getOnuState
echo "== omci_app threads: $(ps | grep -c '[o]mci_app')"
echo "== done $(date)"
