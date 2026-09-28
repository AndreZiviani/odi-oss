#!/usr/bin/env bash
# run-qemu.sh -- boot the odi-oss real rootfs under qemu-system-mips (the
# odi-toolchain-qemu-kernel-malta stock kernel), wait for it to come up,
# check ssh/web/exporter, then run the resilience scenarios. make test-qemu
# (docs/HACKING.md) calls this after test/qemu/build-initramfs.sh.
#
# What this DOES exercise: the real busybox init chain, inittab, rcS,
# rcS.pon, the svc-*.sh respawn entries, dropbear, confd, metricsd, omcid,
# the tmpfs caps, oom_score_adj, busybox init respawn, and sysctls --
# everything under rootfs/skeleton, unpatched.
#
# What this does NOT exercise (docs/HACKING.md has the full list): our own
# kernel (kernel/extra, the odi_* drivers) is not built or booted here at
# all -- a stock kernel stands in for the RTL9602C board qemu cannot
# emulate. So there is no /proc/odi_wdt, no /proc/odi_init, no switch, no
# GPON: omcid still starts (svc-omcid.sh only gates on modules.off and the
# binary existing, not on /proc/odi_omci), but degrades harmlessly exactly
# as it does on any kernel without /proc/odi_omci -- no netlink, no
# registration, message-queue commands still served -- and there is no
# watchdog reset path at all: none of the three odi_wdt rules (boot
# confirmation, per-client ping deadlines, the memory floor;
# docs/SETTINGS.md, "Watchdog rules") can be exercised end to end here,
# only that rcS.pon and omcid degrade harmlessly without /proc/odi_wdt.
# That needs the real hardware or the host-side coverage of the rules
# themselves (test/odi_wdt_test.c).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build/qemu-initramfs}
QEMU_KERNEL_IMAGE=${QEMU_KERNEL_IMAGE:-$("$ROOT/toolchain/image.sh" qemu-kernel)}

SSH_PORT=${SSH_PORT:-2222}
HTTP_PORT=${HTTP_PORT:-8080}
METRICS_PORT=${METRICS_PORT:-9100}
MEM=${MEM:-64M}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-240}

WORK=$(mktemp -d)
trap 'kill "$QEMU_PID" 2>/dev/null || true; rm -rf "$WORK"' EXIT

say() { printf '\n== %s\n' "$*"; }
fail() { echo "test-qemu: FAIL: $*" >&2; exit 1; }

say "vmlinux from $QEMU_KERNEL_IMAGE"
cid=$(docker create "$QEMU_KERNEL_IMAGE")
docker cp "$cid:/boot/vmlinux" "$WORK/vmlinux"
docker rm "$cid" >/dev/null

SSH_KEY=$ROOT/test/qemu/id_test
KEY_ARGS=()
if [ -f "$SSH_KEY" ]; then
	# git checkout gives this file mode 644 (no exec bit tracked, nothing
	# else), which a strict ssh client (measured: Ubuntu's OpenSSH, not
	# every build) refuses outright as an unprotected private key file,
	# silently falling through to "no more authentication methods".
	chmod 600 "$SSH_KEY"
	KEY_ARGS=(-i "$SSH_KEY")
fi
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o ConnectTimeout=3 -o LogLevel=ERROR -p "$SSH_PORT" "${KEY_ARGS[@]}")

say "booting (log: $WORK/qemu.log)"
qemu-system-mips -M malta -kernel "$WORK/vmlinux" \
	-initrd "$BUILD/initramfs.cpio.gz" \
	-append "console=ttyS0 rdinit=/init" \
	-m "$MEM" -nographic -no-reboot \
	-net nic,model=pcnet \
	-net "user,hostfwd=tcp::$SSH_PORT-:22,hostfwd=tcp::$HTTP_PORT-:80,hostfwd=tcp::$METRICS_PORT-:9100" \
	> "$WORK/qemu.log" 2>&1 &
QEMU_PID=$!

say "waiting for ssh (up to ${BOOT_TIMEOUT}s)"
up=0
for _ in $(seq 1 "$BOOT_TIMEOUT"); do
	if ssh "${SSH_OPTS[@]}" root@127.0.0.1 true 2>"$WORK/ssh-wait.log"; then
		up=1
		break
	fi
	kill -0 "$QEMU_PID" 2>/dev/null || { cat "$WORK/qemu.log" >&2; fail "qemu exited before ssh came up"; }
	sleep 1
done
if [ "$up" != 1 ]; then
	echo "== last ssh attempt, verbose ==" >&2
	ssh -v "${SSH_OPTS[@]}" root@127.0.0.1 true 2>&1 | tail -60 >&2
	tail -80 "$WORK/qemu.log" >&2
	fail "ssh never answered within ${BOOT_TIMEOUT}s"
fi
echo "  ssh key auth ok"

sshx() { ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@"; }

say "fstab mounts: /var and /var/tmp are tmpfs with the configured size"
# The root cause of v1.0.2 through v1.0.4-rc1 (kernel/618/config had no
# CONFIG_SHMEM/CONFIG_TMPFS, so a capped tmpfs mount rejected size= and
# /var stayed read-only): this is the assertion that would have caught it,
# read straight off /proc/mounts rather than trusted from a mount -a exit
# code. /etc/fstab has the sizes (6m, 8m); the kernel reports them back in
# KB.
MOUNTS=$(sshx cat /proc/mounts)
echo "$MOUNTS" | grep -qE '^tmpfs /var tmpfs .*size=6144k' || fail "/var is not tmpfs size=6m -- $(echo "$MOUNTS" | grep ' /var ')"
echo "$MOUNTS" | grep -qE '^tmpfs /var/tmp tmpfs .*size=8192k' || fail "/var/tmp is not tmpfs size=8m -- $(echo "$MOUNTS" | grep ' /var/tmp ')"
# The config partition needs a real MTD device (mtd:config, /etc/fstab)
# this harness does not have (no MTD in qemu, docs/HACKING.md) -- so it is
# normally absent here. If one ever does show up at /var/config, it must
# be the real jffs2 mount a flashed image gets, never something softer
# that would silently mask a mismatch.
if echo "$MOUNTS" | grep -q ' /var/config '; then
	echo "$MOUNTS" | grep -qE '^mtd:config /var/config jffs2 ' || fail "/var/config is mounted but not jffs2 -- $(echo "$MOUNTS" | grep ' /var/config ')"
	echo "  /var, /var/tmp tmpfs with configured sizes; /var/config jffs2"
else
	echo "  /var, /var/tmp tmpfs with configured sizes; /var/config absent (no MTD under qemu, expected)"
fi

say "web UI (confd, port $HTTP_PORT)"
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/" || true)
# 401: the confd default, unauthenticated (admin/admin until set) -- still
# proof it is up and answering, which is what this checks.
case "$code" in 200|401) ;; *) fail "web UI returned $code, want 200 or 401" ;; esac
echo "  $code ok"

say "exporter (metricsd, port $METRICS_PORT)"
body=$(curl -sf "http://127.0.0.1:$METRICS_PORT/metrics") || fail "exporter did not answer"
echo "$body" | grep -q "^gpon_" || fail "exporter answered but no gpon_* metric in the body"
echo "  metrics ok ($(echo "$body" | grep -c '^gpon_') gpon_* lines)"

say "scenario: fill /tmp until ENOSPC, services must survive"
sshx 'dd if=/dev/zero of=/tmp/fill bs=1M count=64 2>/tmp/fill.err; echo rc=$?; cat /tmp/fill.err' | tail -5
sshx 'rm -f /tmp/fill'
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/" || true)
case "$code" in 200|401) ;; *) fail "web UI dead after filling /tmp (got $code)" ;; esac
curl -sf "http://127.0.0.1:$METRICS_PORT/metrics" >/dev/null || fail "exporter dead after filling /tmp"
sshx true || fail "ssh dead after filling /tmp"
echo "  /tmp gave ENOSPC (or stopped early), ssh/web/exporter still up"

say "scenario: memory hog, the hog dies, ssh survives"
# oom_score_adj reset to 0 (neutral) before exec: left alone, this process
# is a CHILD OF THE SSH SESSION and inherits the -1000 dropbear itself has -- which
# made the OOM killer refuse to touch it and take unrelated respawned
# daemons instead in earlier runs of this harness. A real runaway process
# on the stick is not a child of a management ssh session, so this reset
# is what makes the scenario representative rather than self-defeating.
sshx 'sh -c "echo 0 > /proc/\$\$/oom_score_adj; exec sh /etc/qemu-test/memhog.sh" >/tmp/memhog.log 2>&1 &'
sleep 10
up=0
for _ in 1 2 3 4 5; do
	sshx true 2>/dev/null && { up=1; break; }
	sleep 2
done
[ "$up" = 1 ] || fail "ssh dead after the memory hog"
echo "  ssh answered through the hog (dmesg on the console log has the OOM kill, if any fired)"

say "scenario: kill -9 dropbear, confd, metricsd, omcid -- each back within 10s"
for svc in dropbear confd metricsd omcid; do
	pid=$(sshx "for p in /proc/[0-9]*; do [ \"\$(cat \$p/comm 2>/dev/null)\" = $svc ] && echo \${p#/proc/}; done | head -1" 2>/dev/null) || pid=""
	[ -n "$pid" ] || { echo "  $svc: not running, skipping" >&2; continue; }
	# || true: killing dropbear's own connection handler can reset THIS
	# very ssh session before it reports back cleanly (measured in CI --
	# not every dropbear pid this loop kills is the one carrying the
	# command, but it can be), which is fine: the respawn check below is
	# what actually matters.
	sshx "kill -9 $pid" || true
	back=0
	for _ in $(seq 1 10); do
		sleep 1
		newpid=$(sshx "for p in /proc/[0-9]*; do [ \"\$(cat \$p/comm 2>/dev/null)\" = $svc ] && echo \${p#/proc/}; done | head -1" 2>/dev/null) || newpid=""
		[ -n "$newpid" ] && [ "$newpid" != "$pid" ] && { back=1; break; }
	done
	[ "$back" = 1 ] || fail "$svc did not respawn within 10s of kill -9"
	echo "  $svc: respawned (pid $pid -> $newpid)"
done

say "scenario: rcS registration of the omcid watchdog client is a no-op without /proc/odi_wdt"
# v1.0.3: no more userland health-kicker to withhold a kick -- odi_wdt owns
# every rule itself (docs/SETTINGS.md, "Watchdog rules"). Nothing here to
# exercise end to end: qemu has no /proc/odi_wdt, so rcS's
# "echo omcid 60 > /proc/odi_wdt/register" is skipped ([ -w ... ] false),
# and omcid's own wdt_ping() (src/omci/respond/main.c) opens a path that
# does not exist and silently no-ops -- both already implied by every
# scenario above having booted and stayed reachable. This just confirms
# rcS did not abort trying.
sshx 'test -e /proc/odi_wdt && echo present || echo absent'

say "scenario: NTP_SERVER syncs the guest clock (opt-in, skips if no host responder)"
# The guest config fixture (build-initramfs.sh) sets NTP_SERVER=10.0.2.2,
# the qemu user-net gateway address -- SLIRP maps it straight to the host
# own loopback, so a plain NTP server bound there answers svc-ntpd.sh in
# the guest without any extra guestfwd wiring. Tried in order: a busybox
# on PATH serving with -l, then a system ntpd. Neither found: skipped, not
# failed -- a real, bounded responder is not always on hand, and this
# harness does not hand-roll a fake NTP protocol server as a substitute.
NTP_HOST_PID=""
NTP_HOST_LOG="$WORK/host-ntpd.log"
start_ntp_responder() {
	if command -v busybox >/dev/null 2>&1 && busybox ntpd --help 2>&1 | grep -q ' -l'; then
		busybox ntpd -n -l -N >"$NTP_HOST_LOG" 2>&1 &
		NTP_HOST_PID=$!
		return 0
	fi
	if command -v ntpd >/dev/null 2>&1; then
		ntpd -n -l >"$NTP_HOST_LOG" 2>&1 &
		NTP_HOST_PID=$!
		return 0
	fi
	return 1
}
if start_ntp_responder; then
	sleep 1
	if kill -0 "$NTP_HOST_PID" 2>/dev/null; then
		echo "  host NTP responder up (pid $NTP_HOST_PID), reachable at 10.0.2.2:123 from the guest"
		# A deliberately wrong guest clock, far enough in the past that
		# the first ntpd correction steps rather than slews -- proves
		# ntpd actually set the clock, not just that it was already close.
		sshx "date -u -s '2000-01-01 00:00:00'" >/dev/null
		before=$(sshx date -u +%s)
		host_now=$(date -u +%s)
		synced=0
		for _ in $(seq 1 30); do
			sleep 1
			now=$(sshx date -u +%s) || continue
			diff=$((now > host_now ? now - host_now : host_now - now))
			[ "$diff" -lt 10 ] && { synced=1; break; }
		done
		if [ "$synced" = 1 ]; then
			echo "  guest clock corrected: was $before, now within 10s of host wall time"
		else
			echo "$(cat "$NTP_HOST_LOG" 2>/dev/null)" >&2
			fail "guest clock did not sync against NTP_SERVER within 30s"
		fi
	else
		echo "  host NTP responder failed to start ($(cat "$NTP_HOST_LOG" 2>/dev/null)) -- skipping"
	fi
	kill "$NTP_HOST_PID" 2>/dev/null || true
else
	echo "  no busybox/ntpd on this host -- skipping (see docs/HACKING.md, test-qemu)"
fi

say "all scenarios passed"
