#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# svc-confd.sh -- the /etc/inittab respawn entry for confd. A safe-mode boot
# (safe-mode.sh, /var/run/safe-mode) starts it whatever confd.off says: that
# boot is the way back into a stick that cannot be reached, and the flag is
# on the config partition, which no reflash clears. The file stays, so the
# next normal boot is as configured.
[ -f /etc/config/confd.off ] && [ ! -f /var/run/safe-mode ] && exec /etc/scripts/respawn-off.sh
[ -x /bin/confd ] || exec /etc/scripts/respawn-off.sh
exec /etc/scripts/respawn.sh confd -500 /var/log/services.log /bin/confd 80
