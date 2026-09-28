#!/bin/sh
# svc-dropbear.sh -- the /etc/inittab respawn entry for dropbear. The key
# setup (mkdir, keygen, the zero-length-key recovery) runs on every start,
# same as it did every boot from /etc/init.d/services before -- idempotent,
# so a respawn after a crash costs nothing extra.
[ -f /etc/config/dropbear.off ] && exec /etc/scripts/respawn-off.sh
[ -x /sbin/dropbear ] || exec /etc/scripts/respawn-off.sh

mkdir -p /etc/config/dropbear.d
KEY=/etc/config/dropbear.d/ed25519
# -s, not -f: a zero-length key is what a power cut during the first-boot
# keygen leaves behind, and dropbear then dies while dropbearkey REFUSES to
# replace an existing file -- so without this the stick loses ssh
# permanently and nothing retries.
[ -e "$KEY" ] && [ ! -s "$KEY" ] && rm -f "$KEY"
# stdout is the public key and is noise; STDERR is not. This is the one
# command here whose failure costs ssh, and it failed silently once already
# -- no /dev/urandom, no key, no message.
[ -s "$KEY" ] || /sbin/dropbearkey -t ed25519 -f "$KEY" >/dev/null
[ -s "$KEY" ] || exec /etc/scripts/respawn-off.sh	# no key: do not spin dropbear with none

# No -E: dropbear logs through syslog by default (svc-syslogd.sh), same as
# every other daemon that can reach it -- this used to run -E, straight to
# STDERR (services.log), back when this image had no syslogd at all.
# -D: public keys for root live beside the host key, on jffs2, so they
# survive a reflash; confd /api/sshkeys writes that file, dropbear rereads
# it on every login, no restart needed. -F: run in the foreground --
# respawn.sh needs the pid it forked to BE dropbear, not a parent that
# daemonises and exits right after. respawn.sh's own services.log redirect
# still catches whatever dropbear itself writes straight to stderr (a
# startup error before logging is set up, say), not its per-session log
# lines, which go through syslog now.
#
# /etc/odi-keys-only is written by image/build.sh only for ROOT_PW=locked
# (docs/BUILDING.md): -s there also refuses password auth at the protocol
# level, so a public release image never offers a password prompt to try
# against.
if [ -f /etc/odi-keys-only ]; then
	exec /etc/scripts/respawn.sh -1000 /var/log/services.log /sbin/dropbear -s -F -r "$KEY" -D /etc/config/dropbear.d -p 22
else
	exec /etc/scripts/respawn.sh -1000 /var/log/services.log /sbin/dropbear -F -r "$KEY" -D /etc/config/dropbear.d -p 22
fi
