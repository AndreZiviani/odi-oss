#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/sfp-host-ddm.sh and its boot step in rcs-lib.sh
# (pon_step_sfphostddm), off the device (SFP_HOST_DDM).
#
# The kernel parameters are plain files here. A write of the sfp_mirror file
# is answered by a stub kernel, run by the timeout stub after each command,
# which moves sfp_mirror_state the way odi_sfp_mirror.c does: 1 makes it
# filling and, after $T/fill_s more commands, on (or failed when $T/fail
# exists); 0 makes it off. The real flash wrapper stores the key, in a
# temporary odi.conf.
#
# What is checked: the key parsing (absent, empty, 1, 0, anything else), the
# boot (default on, off, a safe-mode boot, no kernel parameter), apply (on,
# off, the kernel failing, the bounded wait), status, the apply.sh verb, and
# where the boot step sits among the PON steps.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/rootfs/skeleton/etc/scripts/sfp-host-ddm.sh
F=$PWD/rootfs/skeleton/etc/scripts/flash
A=$PWD/rootfs/skeleton/etc/scripts/apply.sh
LIB=$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
# t NAME PATTERN TEXT: TEXT matches the extended regex PATTERN.
t() { if [[ $3 =~ $2 ]]; then ok "$1"; else no "$1" "wanted /$2/, got: $3"; fi; }
# n NAME PATTERN TEXT: TEXT does not match.
n() { if [[ $3 =~ $2 ]]; then no "$1" "did not want /$2/, got: $3"; else ok "$1"; fi; }

mkdir -p "$T/bin" "$T/config" "$T/run" "$T/param"
# The stub kernel: a write to sfp_mirror leaves its value in the file; this
# turns it into the state the driver would show.
cat > "$T/bin/kernel" <<EOF
#!/bin/sh
p=$T/param
[ -f "\$p/sfp_mirror" ] || exit 0
v=\$(cat "\$p/sfp_mirror")
case \$v in
1) echo Y > "\$p/sfp_mirror"; echo filling > "\$p/sfp_mirror_state"; echo 0 > "$T/ticks" ;;
0) echo N > "\$p/sfp_mirror"; echo off > "\$p/sfp_mirror_state" ;;
esac
if [ "\$(cat "\$p/sfp_mirror_state")" = filling ]; then
	n=\$(( \$(cat "$T/ticks") + 1 )); echo \$n > "$T/ticks"
	if [ "\$n" -gt "\$(cat "$T/fill_s")" ]; then
		if [ -f "$T/fail" ]; then echo failed; else echo on; fi > "\$p/sfp_mirror_state"
	fi
fi
EOF
cat > "$T/bin/timeout" <<EOF
#!/bin/sh
shift
"\$@"; rc=\$?
"$T/bin/kernel"
exit \$rc
EOF
cat > "$T/bin/sleep" <<EOF
#!/bin/sh
echo "\$1" >> "$T/slept"
"$T/bin/kernel"
EOF
chmod +x "$T/bin/"*

export PATH="$T/bin:$PATH"
export FLASH=$F PARAM_DIR=$T/param SAFE_FLAG=$T/run/safe-mode KMSG=$T/kmsg
export TIMEOUT=$T/bin/timeout SLEEP=$T/bin/sleep APPLY_WAIT=5
export ODI_CONF=$T/config/odi.conf CS=$T/config/lastgood.xml HS=$T/config/lastgood_hs.xml TMPDIR_=$T

setkey() { "$F" set SFP_HOST_DDM "$1" > /dev/null; }
# A fresh boot: the parameter as the kernel starts it, nothing logged.
newboot() {
	rm -rf "$T/run" "$T/slept" "$T/fail"
	mkdir -p "$T/run" "$T/param"
	echo N > "$T/param/sfp_mirror"
	echo off > "$T/param/sfp_mirror_state"
	echo "${1:-1}" > "$T/fill_s"
	echo 0 > "$T/ticks"
	: > "$T/kmsg"
}

# ---- boot, key absent: on by default
newboot
out=$(sh "$S" boot); rc=$?
[ "$rc" = 0 ] && ok "default boot: exit 0" || no "default boot: exit 0" "rc $rc"
t "default boot: the parameter is written 1" "^Y$" "$(cat "$T/param/sfp_mirror")"
t "default boot: event=requested setting=on" "event=requested setting=on" "$(cat "$T/kmsg")"
t "default boot: at notice" "^<5>sfp-host-ddm: " "$(cat "$T/kmsg")"
t "default boot: no wait" "^$" "$(cat "$T/slept" 2>/dev/null)"
t "default boot: says it on stdout" "event=requested" "$out"

# ---- the key
for v in 1 "" 0 yes on 2; do
	newboot
	if [ -n "$v" ]; then setkey "$v"; else setkey ""; fi
	sh "$S" boot > /dev/null
	case $v in
	0) t "key '0': left off" "^N$" "$(cat "$T/param/sfp_mirror")"
	   t "key '0': event=off" "event=off setting=off" "$(cat "$T/kmsg")" ;;
	*) t "key '$v': on" "^Y$" "$(cat "$T/param/sfp_mirror")" ;;
	esac
	case $v in
	yes|on|2) t "key '$v': event=bad_value" "event=bad_value setting=on SFP_HOST_DDM=$v" "$(cat "$T/kmsg")" ;;
	*)        n "key '$v': no bad_value" "bad_value" "$(cat "$T/kmsg")" ;;
	esac
done
setkey ""
t "an empty value removes the key" "^$" "$(grep SFP_HOST_DDM "$ODI_CONF" 2>/dev/null)"

# ---- a safe-mode boot leaves it off whatever the key says
newboot
: > "$T/run/safe-mode"
sh "$S" boot > /dev/null
t "safe-mode boot: left off" "^N$" "$(cat "$T/param/sfp_mirror")"
t "safe-mode boot: event=safe_mode_boot" "event=safe_mode_boot setting=on .*SFP_HOST_DDM=unset" "$(cat "$T/kmsg")"

# ---- a kernel without the parameter
newboot
rm -f "$T/param/sfp_mirror"
out=$(sh "$S" boot); rc=$?
[ "$rc" = 0 ] && ok "no kernel: boot still exits 0" || no "no kernel: boot exits 0" "rc $rc"
t "no kernel: event=no_kernel" "event=no_kernel" "$(cat "$T/kmsg")"

# ---- apply: on, waits for the kernel
newboot 2
out=$(sh "$S" apply); rc=$?
[ "$rc" = 0 ] && ok "apply on: exit 0" || no "apply on: exit 0" "rc $rc"
t "apply on: event=applied when the kernel says on" "event=applied setting=on host EEPROM on" "$out"
t "apply on: the state is on" "^on$" "$(cat "$T/param/sfp_mirror_state")"
t "apply on: waited for the fill" "1" "$(cat "$T/slept" 2>/dev/null)"

# ---- apply: off
setkey 0
out=$(sh "$S" apply); rc=$?
[ "$rc" = 0 ] && ok "apply off: exit 0" || no "apply off: exit 0" "rc $rc"
t "apply off: the parameter is written 0" "^N$" "$(cat "$T/param/sfp_mirror")"
t "apply off: state off" "^off$" "$(cat "$T/param/sfp_mirror_state")"
t "apply off: event=applied" "event=applied setting=off host EEPROM off" "$out"
setkey ""

# ---- apply: the kernel fails
newboot 1
: > "$T/fail"
out=$(sh "$S" apply); rc=$?
[ "$rc" = 1 ] && ok "apply failed: exit 1" || no "apply failed: exit 1" "rc $rc"
t "apply failed: event=apply_failed" "event=apply_failed .*dmesg" "$out"

# ---- apply: the kernel never answers; the wait is bounded
newboot 1000
out=$(sh "$S" apply); rc=$?
[ "$rc" = 1 ] && ok "apply timeout: exit 1" || no "apply timeout: exit 1" "rc $rc"
t "apply timeout: event=apply_timeout" "event=apply_timeout .*still filling after 5 s" "$out"
t "apply timeout: five one-second waits" "^5$" "$(grep -c . "$T/slept")"

# ---- status
newboot 0
sh "$S" boot > /dev/null
"$T/bin/kernel"
out=$(sh "$S" status)
t "status: setting" "setting=on" "$out"
t "status: request" "request=Y" "$out"
t "status: state" "state=on" "$out"

# ---- apply.sh sfp
newboot 0
out=$(SFP_HOST_DDM_SH=$S sh "$A" sfp); rc=$?
[ "$rc" = 0 ] && ok "apply.sh sfp: exit 0" || no "apply.sh sfp: exit 0" "rc $rc"
t "apply.sh sfp: runs sfp-host-ddm.sh apply" "event=applied setting=on" "$out"
newboot 1
: > "$T/fail"
SFP_HOST_DDM_SH=$S sh "$A" sfp > /dev/null; rc=$?
[ "$rc" = 1 ] && ok "apply.sh sfp: a failure is exit 1" || no "apply.sh sfp: a failure is exit 1" "rc $rc"

# ---- the boot step
order=$(sed -n '/^pon_steps_default()/,/^}/p' "$LIB" | grep -n 'pon_step_laser\|pon_step_sfphostddm\|pon_step gpon$' | sed 's/^[0-9]*:[[:space:]]*//' | tr '\n' ' ')
t "boot step: after the laser step, before gpon" "^pon_step_laser pon_step_sfphostddm pon_step gpon $" "$order"
newboot
out=$(
	crumb() { echo "crumb $*"; }
	# shellcheck source=/dev/null
	. "$LIB"
	crumb() { echo "crumb $*"; }
	SFP_HOST_DDM_SH=$S pon_step_sfphostddm
)
t "boot step: runs boot, quietly" "^crumb pon step sfp-host-ddm: start
crumb pon step sfp-host-ddm: done$" "$out"
t "boot step: the parameter is written" "^Y$" "$(cat "$T/param/sfp_mirror")"
grep -q 'SFP_HOST_DDM' "$F" && ok "flash lists SFP_HOST_DDM as an odi-only key" || no "flash lists SFP_HOST_DDM" "missing"

echo "sfp_host_ddm_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
