# shellcheck shell=sh
#
# rcs-lib.sh -- functions shared by /etc/init.d/rcS (sysinit) and
# /etc/init.d/rcS.pon (the `once` entry that runs the PON steps after it,
# docs/BOOT.md). Split out so both scripts see the same config_mounted,
# crumb and confirm_watchdog logic rather than two copies drifting apart.
# rcS.dev, when present, is sourced by BOTH callers too and redefines crumb,
# dev_hook, dev_confirm_ok and pon_steps -- without it every one of those is
# the plain default below.

kmsg() { echo "rcS: $*" > /dev/kmsg 2>/dev/null || true; }
crumb() { kmsg "$*"; }

# Bounded write into a driver /proc verb file (odi_init, odi_omci, and
# friends). A bare `echo x > file` is a write(2) the calling shell performs
# directly, not a forked command -- `timeout` only bounds a process it
# starts, so the write has to run inside one for `timeout` to reach it at
# all. This is a documented limit, not a guarantee: a write the kernel driver
# blocks on in D state (uninterruptible sleep) ignores every signal,
# including the one `timeout` sends -- see rcS.dev, the note on why a diag
# ioctl gets no `timeout` wrapper there either. It still catches the ordinary
# case (a driver waiting on a lock or a condition that resolves on its own),
# which is what "odi_init step hangs the boot" would actually look like.
#
# The line goes through positional parameters ($1/$2 inside the inner shell),
# not string interpolation into the script text, so a value containing shell
# metacharacters (a password from the config store, say) is never re-parsed.
PROC_WRITE_TIMEOUT_S=${PROC_WRITE_TIMEOUT_S:-5}
write_proc_bounded() {
	path=$1 line=$2
	timeout "$PROC_WRITE_TIMEOUT_S" sh -c 'echo "$1" > "$2"' _ "$line" "$path"
}
read_proc_bounded() {
	path=$1
	timeout "$PROC_WRITE_TIMEOUT_S" cat "$path" 2>/dev/null
}
dev_hook() { :; }
dev_confirm_ok() { return 0; }
pon_steps() { pon_steps_default; }	# the rcS.dev pon-steps override replaces this

# The one place anything asks "is the config partition actually mounted".
# The live mount table, checked fresh every call -- see rcS (this used to be
# defined only there; unchanged).
CONFIG_DIR=/var/config
config_mounted() {
	grep -qE "^[^ ]+ $CONFIG_DIR jffs2 " /proc/mounts 2>/dev/null && [ -d /etc/config ]
}

required_mounts_ok() {
	ok=1
	for m in /var /var/tmp; do
		grep -qE "^[^ ]+ $m tmpfs " /proc/mounts 2>/dev/null || {
			kmsg "required mount missing: $m (tmpfs)"
			ok=0
		}
	done
	[ "$ok" = 1 ]
}

# ---- the PON steps (docs/BOOT.md, "The PON steps"). Run from rcS.pon, the
# `once` inittab entry started right after sysinit returns -- concurrently
# with the respawn entries (dropbear, confd, metricsd, omcid) init starts in
# the same breath, exactly as these ran concurrently with the backgrounded
# `services start` before v1.0.4 (rcS launched it with a trailing `&` and
# moved straight on to the PON steps without waiting for it either).

pon_step() {
	crumb "pon step ${1%% *}: start"
	write_proc_bounded /proc/odi_init "$1" 2>/dev/null || \
		echo "rcS: odi_init ${1%% *} failed to start" >&2
	crumb "pon step ${1%% *}: ret $(read_proc_bounded /proc/odi_init)"
}

# The serial number (an HS key) and the PLOAM password (hex, in the CS
# file) come from the config store, never from a file of ours. An empty
# password (an OLT that authenticates on the serial alone) is not sent:
# the driver keeps its default.
pon_step_gponsn() {
	pon_step "gponsn $(sed -n "s/.*Name=\"GPON_SN\" Value=\"\([^\"]*\)\".*/\1/p" /etc/config/lastgood_hs.xml 2>/dev/null | head -n 1)"
}
pon_step_gponpw() {
	pw=$(sed -n "s/.*Name=\"GPON_PLOAM_PASSWD\" Value=\"\([^\"]*\)\".*/\1/p" /etc/config/lastgood.xml 2>/dev/null | head -n 1)
	if [ -n "$pw" ]; then
		pon_step "gponpw $pw"
	else
		crumb "pon step gponpw: none configured, skipped"
	fi
}

# gponact activates the ONU, and the OLT can start sending OMCI frames the
# instant it does -- so omcid must already be registered with odi_omci
# (redirect type 1) or the first frames are dropped_unregistered
# (kernel/extra/drivers/net/ethernet/odi/odi_omci.c). Before v1.0.4, omcid
# was forked right here, inline, so it had already called nl_redirect()
# (its own registration) by the time this ran; now it starts as its own
# respawn entry, concurrently with this whole script, so its startup can
# lag this point by an unpredictable (if usually small) amount. Poll the
# live registration table instead of guessing a fixed delay -- bounded, so
# a stuck or missing omcid still lets activation happen rather than hang
# the boot.
pon_step_gponact() {
	i=0
	while [ "$i" -lt 10 ]; do
		grep -q "type=1 " /proc/odi_omci 2>/dev/null && break
		sleep 1
		i=$((i + 1))
	done
	if grep -q "type=1 " /proc/odi_omci 2>/dev/null; then
		crumb "gponact: omcid registered with odi_omci after ${i}s"
	else
		crumb "gponact: omcid NOT registered with odi_omci after 10s, activating anyway"
	fi
	pon_step gponact
}

# The switch init the stock OMCI modules made when they loaded (platform
# settings, module-load replay). /etc/config/modules.off skips it (and, as
# before, the PON activation steps still run -- only the switch program and
# the SWITCH_INIT_OK gate below are skipped). omcid itself no longer starts
# here: it is /etc/scripts/svc-omcid.sh, a respawn entry in /etc/inittab
# (docs/SETTINGS.md, "native over hand-rolled" -- supervise.sh is gone).
omci_start() {
	if [ -f /etc/config/modules.off ]; then
		crumb "omci: modules.off, switch init skipped"
		return 0
	fi
	if [ ! -w /proc/odi_omci ]; then
		crumb "omci: no /proc/odi_omci, switch init skipped"
		return 0
	fi
	if write_proc_bounded /proc/odi_omci switch_init 2>/var/log/switch_init.err; then
		crumb "odi_switch init: platform settings and module-load replay done"
	else
		SWITCH_INIT_OK=0
		crumb "odi_switch init: /proc/odi_omci write failed, see /var/log/switch_init.err"
	fi
}

# ---- the watchdog confirmation. See docs/BOOT.md, "The watchdog
# confirmation" -- unchanged by the respawn split: this never depended on
# omcid actually being up, only on switch_init not having failed
# (SWITCH_INIT_OK), the config and required mounts, and a real management
# address, all of which are decided inside this same script by the time it
# runs.
WDOK=/proc/odi_wdt/userland_ok
SWITCH_INIT_OK=1
confirm_watchdog() {
	[ -w "$WDOK" ] || return 0
	if ! required_mounts_ok; then
		crumb "watchdog NOT confirmed: a required mount is missing"
		return 0
	fi
	if ! config_mounted; then
		crumb "watchdog NOT confirmed: config partition not mounted"
		return 0
	fi
	if ! /etc/scripts/network.sh configured >/dev/null 2>&1; then
		crumb "watchdog NOT confirmed: no management address configured"
		return 0
	fi
	if [ "$SWITCH_INIT_OK" != 1 ]; then
		crumb "watchdog NOT confirmed: switch_init failed"
		return 0
	fi
	dev_confirm_ok || return 0
	echo 1 > "$WDOK"
	crumb "userland up, watchdog confirmed"
}

# The order is fixed by the hardware (docs/BOOT.md). The switch init goes
# after gpondev: gpondrv/gpondev clears the table the omcid receive path
# registers in, so the switch program (and, in the respawn world, the
# omcid startup itself) must not race ahead of it; the switch init
# deactivates the ONU, so gponsn, gponpw and gponact -- gated on omcid,
# above -- come after it.
pon_steps_default() {
	pon_step "i2c 1"
	pon_step "i2cen 1"
	pon_step gpon
	pon_step rxsd
	pon_step gpondrv
	pon_step gpondev
	omci_start
	confirm_watchdog
	pon_step_gponsn
	pon_step_gponpw
	pon_step_gponact
}
