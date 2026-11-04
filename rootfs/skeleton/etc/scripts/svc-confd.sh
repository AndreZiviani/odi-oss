#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# svc-confd.sh -- the /etc/inittab respawn entry for confd.
[ -f /etc/config/confd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /bin/confd ] || exec /etc/scripts/respawn-off.sh
exec /etc/scripts/respawn.sh -500 /var/log/services.log /bin/confd 80
