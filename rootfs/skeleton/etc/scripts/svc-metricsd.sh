#!/bin/sh
# svc-metricsd.sh -- the /etc/inittab respawn entry for metricsd. Checked on
# every start (a fresh exec, not only at boot), so `services stop metricsd`
# (writes the .off flag and kills the running process) takes effect the
# moment init restarts this entry, no reboot needed.
[ -f /etc/config/metricsd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /bin/metricsd ] || exec /etc/scripts/respawn-off.sh
# oom_score_adj left at the kernel default (0): the one daemon whose loss
# costs neither management access nor GPON state, so it is first in line
# for the OOM killer if it has to take something at all (docs/SETTINGS.md).
exec /etc/scripts/respawn.sh 0 /var/log/services.log /bin/metricsd 9100
