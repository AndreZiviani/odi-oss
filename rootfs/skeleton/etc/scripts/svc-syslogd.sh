#!/bin/sh
# svc-syslogd.sh -- the /etc/inittab respawn entry for syslogd. Checked on
# every start (a fresh exec, not only at boot), so a config change or
# `apply.sh syslog` (docs/SETTINGS.md, SYSLOG_SERVER) takes effect the
# moment init restarts this entry, no reboot needed -- same idiom as
# svc-metricsd.sh and every other svc-*.sh here.
[ -f /etc/config/syslogd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /sbin/syslogd ] || exec /etc/scripts/respawn-off.sh
# The store read goes through flash, the one accessor: SYSLOG_SERVER and
# NTP_SERVER are odi-only keys that flash keeps in /etc/config/odi.conf, not in
# the stock XML (see flash). `flash get` prints KEY=value; no value, no output.
config_get() {
	/etc/scripts/flash get "$1" 2>/dev/null | sed 's/^[^=]*=//' | grep .
}

SERVER=$(config_get SYSLOG_SERVER || true)

# -n: foreground, the pid respawn.sh execs into IS syslogd (docs/SETTINGS.md,
# "native over hand-rolled" -- init is the supervisor, nothing here may fork
# and let its child dangle). -C64: a 64 KB circular buffer in shared memory
# (packages/busybox/config.fragment turns the feature on), not a log file --
# `logread` reads it, and it cannot fill the config or root partition. 64 KB
# holds several boots worth of service and kernel messages on a device this
# small without competing with the rest of RAM.
if [ -n "$SERVER" ]; then
	# -R host[:port] forwards a copy remotely; -L keeps the local circular
	# buffer too, so `logread` still works when the remote collector is not
	# reachable.
	exec /etc/scripts/respawn.sh 0 /var/log/services.log /sbin/syslogd -n -C64 -L -R "$SERVER"
else
	exec /etc/scripts/respawn.sh 0 /var/log/services.log /sbin/syslogd -n -C64
fi
