#!/bin/sh
# svc-omcid.sh -- the /etc/inittab respawn entry for omcid. Used to start
# inline, deep in rcS PON steps, forked by supervise.sh; now its own
# respawn entry, so its startup runs concurrently with rcS.pon (rcs-lib.sh
# pon_step_gponact waits, bounded, for this to register with odi_omci
# before activating the ONU). -a: apply mode (program the switch). -d: the
# daemon form -- no frame or idle limit, stopped only by a signal, still
# runs in the foreground (src/omci/respond/main.c never forks or
# daemonizes, whichever flags are given -- this always was safe to exec in
# place; the old failure to respawn on hardware was supervise.sh losing
# track of the child, not omcid double-forking).
[ -f /etc/config/modules.off ] && exec /etc/scripts/respawn-off.sh
[ -x /bin/omcid ] || exec /etc/scripts/respawn-off.sh

# Respawn re-provisioning (docs/BOOT.md): a marker under /var/run, tmpfs
# and gone at the next reboot, tells this apart from the first start of the
# boot -- rcS.pon's own gponact already provisions that one. Any later
# start means init just respawned a dead omcid (rc5 hardware finding: the
# OLT had already provisioned this ONU and does not re-send the MIB to a
# fresh omcid with an empty one, so nothing else brings services back).
# Kick off the re-provisioning in the background rather than here: it has
# to wait for the omcid this same line is about to start, and svc-omcid.sh
# itself must return control to init's exec below without waiting on it.
MARKER=${SVC_OMCID_MARKER:-/var/run/svc-omcid.started}
if [ -e "$MARKER" ]; then
	/etc/scripts/omci-respawn-reprovision.sh &
else
	: > "$MARKER" 2>/dev/null
fi

exec /etc/scripts/respawn.sh -1000 /var/log/omcid.log /bin/omcid -a -d
