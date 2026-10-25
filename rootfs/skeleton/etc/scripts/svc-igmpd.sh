#!/bin/sh
# svc-igmpd.sh -- the /etc/inittab respawn entry for igmpd. Opt-in: nothing
# runs unless IGMP_SNOOPING=1 is set in the config store (docs/SETTINGS.md),
# and with it set igmpd still does nothing until the OLT provisions a
# multicast operations profile that asks for snooping (omcid writes it to
# /var/run/odi-mcast.conf). Without the daemon the switch floods multicast
# exactly as it always did, so the default image carries no snooping risk.
#
# The inittab entry is static (busybox init reads it once, at boot), so the
# gate lives here, checked fresh on every restart, the same idiom as every
# other svc-*.sh: setting the key and running `apply.sh igmp` starts the
# daemon with no reboot, clearing it stops it the same way.
[ -f /etc/config/modules.off ] && exec /etc/scripts/respawn-off.sh
[ -f /etc/config/igmpd.off ] && exec /etc/scripts/respawn-off.sh
[ -x /bin/igmpd ] || exec /etc/scripts/respawn-off.sh

# The store read goes through flash, the one accessor: IGMP_SNOOPING is an
# odi-only key that flash keeps in /etc/config/odi.conf. `flash get` prints
# KEY=value, KEY= when unset; the grep turns an empty value into no output.
VALUE=$(/etc/scripts/flash get IGMP_SNOOPING 2>/dev/null | sed 's/^[^=]*=//' | grep . || true)
[ "$VALUE" = "1" ] || exec /etc/scripts/respawn-off.sh

# IGMP_PON_DROP says whether the PON also drops IPv4 multicast no row names,
# the one part that can cut a stream. Strictly opt-in: only 1 asks for it,
# unset or anything else snoops without dropping.
DROP=$(/etc/scripts/flash get IGMP_PON_DROP 2>/dev/null | sed 's/^[^=]*=//' | grep . || true)
FLAGS=-w
[ "$DROP" = "1" ] && FLAGS="-w -d"

# -w: program the switch. The daemon takes its snooping lease on the switch
# only while the OLT asks for snooping, and gives it back the moment that
# stops or the daemon dies. oom_score_adj 0: it is no more important than the
# other services, and less than omcid.
# shellcheck disable=SC2086 # FLAGS is two words on purpose
exec /etc/scripts/respawn.sh 0 /var/log/services.log /bin/igmpd $FLAGS
