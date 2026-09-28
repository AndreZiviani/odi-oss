#!/bin/sh
# respawn-off.sh -- the placeholder a per-daemon respawn entry execs into
# when its /etc/config/<name>.off flag is set (docs/SETTINGS.md). init
# still restarts this every time it exits, same as any other respawn
# entry, but it only waits -- cheaply, and `/etc/init.d/services start`
# kills it by this same script path (in its /proc/<pid>/cmdline) to bring
# a stopped daemon back without a reboot.
while :; do
	sleep 3600
done
