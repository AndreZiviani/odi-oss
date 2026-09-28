#!/bin/sh
# svc-confd.sh -- the /etc/inittab respawn entry for confd.
[ -f /etc/config/confd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /bin/confd ] || exec /etc/scripts/respawn-off.sh
exec /etc/scripts/respawn.sh -500 /var/log/services.log /bin/confd 80
