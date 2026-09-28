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
exec /etc/scripts/respawn.sh -1000 /var/log/omcid.log /bin/omcid -a -d
