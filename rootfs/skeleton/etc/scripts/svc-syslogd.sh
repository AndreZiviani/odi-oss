#!/bin/sh
# svc-syslogd.sh -- the /etc/inittab respawn entry for syslogd. Checked on
# every start (a fresh exec, not only at boot), so a config change or
# `apply.sh syslog` (docs/SETTINGS.md, SYSLOG_SERVER) takes effect the
# moment init restarts this entry, no reboot needed -- same idiom as
# svc-metricsd.sh and every other svc-*.sh here.
[ -f /etc/config/syslogd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /sbin/syslogd ] || exec /etc/scripts/respawn-off.sh

# The config store read, same sed-on-XML-attribute approach network.sh
# uses for LAN_IP_ADDR: CS first, then HS (docs/SETTINGS.md, "omcid looks
# for each key in both files").
config_get() {
	for f in /var/config/lastgood.xml /var/config/lastgood_hs.xml; do
		[ -f "$f" ] || continue
		v=$(sed -n "s/.*Name=\"$1\" Value=\"\([^\"]*\)\".*/\1/p" "$f" 2>/dev/null | head -n 1)
		[ -n "$v" ] && { printf '%s\n' "$v"; return 0; }
	done
	return 1
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
