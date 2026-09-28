#!/bin/sh
# omci-respawn-reprovision.sh -- run in the background by svc-omcid.sh
# whenever init has just respawned omcid (not the first start of this
# boot). See docs/BOOT.md, "Respawn re-provisioning": omcid respawns and
# serves omcicli again within seconds of a kill, but an already-provisioned
# OLT does not re-send the MIB to a fresh omcid with an empty one, so
# gpon_omci_services stays 0 indefinitely on its own.
#
# Forces the OLT to re-range and re-provision the same way the proven
# `apply.sh omci` restart does -- deactivate, wait for the (already
# respawned) omcid to register, re-apply the PLOAM password, hold,
# reactivate (rcs-lib.sh's omci_reactivate, the same tail apply.sh uses) --
# without touching the omcid process itself: init is already supervising
# it as a respawn entry, so killing or starting one from here would only
# trigger another respawn, and another run of this script.
set -u

RCS_LIB=${RCS_LIB:-$(dirname "$0")/rcs-lib.sh}
# shellcheck source=./rcs-lib.sh
. "$RCS_LIB"

ODI_INIT=${ODI_INIT:-/proc/odi_init}
REGISTER_WAIT=${REGISTER_WAIT:-10}  # the respawned omcid registers for redirect type 1
DEACT_HOLD=${DEACT_HOLD:-3}         # deactivated at least this long, as measured

# No PON verbs on this kernel (test/qemu/run-qemu.sh's stock kernel, or a
# dev image with modules.off) -- nothing to reprovision.
[ -w "$ODI_INIT" ] || exit 0

t0=$(uptime_s)
if ! write_proc_bounded "$ODI_INIT" gpondeact 2>/dev/null; then
	crumb "omci respawn: gpondeact failed, leaving the ONU as it is"
	exit 1
fi
crumb "omci respawn: ONU deactivated, waiting for the respawned omcid to register"

new=$(omci_reactivate "$t0" "$REGISTER_WAIT" "$DEACT_HOLD") || {
	crumb "omci respawn: gponact failed, the ONU stays off the line until a reboot"
	exit 1
}
if [ -n "$new" ]; then
	crumb "omci respawn: omcid $new registered, ONU reactivated"
else
	crumb "omci respawn: omcid did not register within ${REGISTER_WAIT}s, activated anyway"
fi
