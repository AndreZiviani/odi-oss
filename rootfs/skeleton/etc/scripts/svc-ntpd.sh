#!/bin/sh
# svc-ntpd.sh -- the /etc/inittab respawn entry for ntpd. Opt-in: the
# stock image has no NTP client at all, and this one starts ntpd only when NTP_SERVER is set in the
# config store (docs/SETTINGS.md). The inittab entry is static (busybox
# init reads it once, at boot), so the gate lives here, in the script this
# same respawn entry always execs into -- the standard off-flag idiom
# every svc-*.sh here uses, checked fresh on every restart, not just at
# boot: setting NTP_SERVER and running `apply.sh ntp` starts it with no
# reboot, and clearing it stops it the same way.
[ -f /etc/config/ntpd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /bin/ntpd ] || exec /etc/scripts/respawn-off.sh

# The store read goes through flash, the one accessor: SYSLOG_SERVER and
# NTP_SERVER are odi-only keys that flash keeps in /etc/config/odi.conf, not in
# the stock XML (see flash). `flash get` prints KEY=value; no value, no output.
config_get() {
	/etc/scripts/flash get "$1" 2>/dev/null | sed 's/^[^=]*=//' | grep .
}

SERVER=$(config_get NTP_SERVER || true)
# No server configured: respawn-off.sh, the same "exists but does nothing"
# placeholder every other off flag uses, rather than a bare `exit 0` that
# would have init respawn this script in a tight loop.
[ -n "$SERVER" ] || exec /etc/scripts/respawn-off.sh

# -n: foreground. -p: the one server this device is configured with; no -q,
# so ntpd keeps disciplining the clock for as long as it runs, rather than
# setting it once and exiting (which would just have init respawn it,
# stepping the clock repeatedly instead of slewing it).
exec /etc/scripts/respawn.sh 0 /var/log/services.log /bin/ntpd -n -p "$SERVER"
