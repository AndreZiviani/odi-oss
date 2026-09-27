#!/bin/sh
# health-kicker.sh -- periodic confirmation to /proc/odi_wdt/health_kick
# (docs/SETTINGS.md, kernel/extra/drivers/net/ethernet/odi/odi_wdt.c):
# independent of the one-shot boot confirmation rcS already writes to
# /proc/odi_wdt/userland_ok. Started by rcS as a supervised process
# (supervise.sh) after that boot confirmation; a supervisor restart of
# THIS script does not change what the kernel side does with a late kick --
# a kick that arrives past health_period is a miss regardless of why.
#
# Kicks only while BOTH hold:
#   - omcid is running, unless /etc/config/modules.off says it should not be
#     (the same flag the omci_start() function in rcS reads -- a stick with OMCI turned
#     off intentionally must not be reset for omcid being absent);
#   - MemAvailable is at or above HEALTH_MEM_FLOOR_KB.
#
# A miss on either forces a reset (through odi_wdt, wdt_pre_reset_hook
# quiesces the NIC DMA first) instead of leaving the stick reachable but
# degraded: an OOM that takes dropbear, confd and omcid without any of
# them coming back leaves the hardware datapath forwarding on its own but
# the box otherwise unmanageable, exactly the case this resets out of.
set -u

HEALTH_KICK=/proc/odi_wdt/health_kick
HEALTH_PERIOD_S=${HEALTH_PERIOD_S:-30}
HEALTH_MEM_FLOOR_KB=${HEALTH_MEM_FLOOR_KB:-2048}

# Idle forever, rather than exit, on a kernel without /proc/odi_wdt (any
# kernel but this repo's own -- the qemu test harness included): exiting
# here would have supervise() (rootfs/skeleton/etc/scripts/supervise.sh)
# treat a plain "nothing to do" as a crash and burn its whole restart
# budget doing nothing, uselessly, in the first couple of minutes of boot.
if [ ! -w "$HEALTH_KICK" ]; then
	while :; do sleep 3600; done
fi

omcid_ok() {
	[ -f /etc/config/modules.off ] && return 0
	for p in /proc/[0-9]*; do
		[ "$(cat "$p/comm" 2>/dev/null)" = "omcid" ] && return 0
	done
	return 1
}

mem_ok() {
	avail=$(sed -n 's/^MemAvailable:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo 2>/dev/null)
	[ -n "$avail" ] && [ "$avail" -ge "$HEALTH_MEM_FLOOR_KB" ]
}

while :; do
	sleep "$HEALTH_PERIOD_S"
	if omcid_ok && mem_ok; then
		echo 1 > "$HEALTH_KICK" 2>/dev/null || true
	else
		echo "health-kicker: withholding kick (omcid_ok=$(omcid_ok && echo 1 || echo 0) mem_ok=$(mem_ok && echo 1 || echo 0))" > /dev/kmsg 2>/dev/null || true
	fi
done
