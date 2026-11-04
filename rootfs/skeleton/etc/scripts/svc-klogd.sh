#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# svc-klogd.sh -- the /etc/inittab respawn entry for klogd. Forwards kernel
# log messages into the syslogd circular buffer (svc-syslogd.sh); it has
# no settings of its own, so nothing here reads the config store.
[ -f /etc/config/klogd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /sbin/klogd ] || exec /etc/scripts/respawn-off.sh

# -n: foreground, same reason as svc-syslogd.sh.
exec /etc/scripts/respawn.sh 0 /var/log/services.log /sbin/klogd -n
