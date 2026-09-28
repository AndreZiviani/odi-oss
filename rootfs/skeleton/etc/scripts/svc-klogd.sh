#!/bin/sh
# svc-klogd.sh -- the /etc/inittab respawn entry for klogd. Forwards kernel
# log messages into the syslogd circular buffer (svc-syslogd.sh); it has
# no settings of its own, so nothing here reads the config store.
[ -f /etc/config/klogd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /sbin/klogd ] || exec /etc/scripts/respawn-off.sh

# -n: foreground, same reason as svc-syslogd.sh.
exec /etc/scripts/respawn.sh 0 /var/log/services.log /sbin/klogd -n
