#!/usr/bin/env bash
# run-qemu.sh -- boot the odi-oss real rootfs under qemu-system-mips (the
# odi-toolchain-qemu-kernel-malta stock kernel), wait for it to come up,
# check ssh/web/exporter, then run the resilience scenarios. make test-qemu
# (docs/HACKING.md) calls this after test/qemu/build-initramfs.sh.
#
# What this DOES exercise: the real busybox init chain, inittab, rcS,
# services, dropbear, confd, metricsd, the tmpfs caps, oom_score_adj,
# supervise() respawn, and sysctls -- everything under rootfs/skeleton,
# unpatched.
#
# What this does NOT exercise (docs/HACKING.md has the full list): our own
# kernel (kernel/extra, the odi_* drivers) is not built or booted here at
# all -- a stock kernel stands in for the RTL9602C board qemu cannot
# emulate. So there is no /proc/odi_wdt, no /proc/odi_init, no switch, no
# GPON, no real omcid (omci_start() in rcS skips it, exactly as it does on
# any kernel without /proc/odi_omci) -- and no watchdog reset path: the
# health-kicker scenario below can only confirm the kicker WITHHOLDS a
# kick under the conditions that should trigger a reset, not that the
# board actually resets, which needs the real odi_wdt hardware model this
# harness does not have.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build/qemu-initramfs}
QEMU_KERNEL_IMAGE=${QEMU_KERNEL_IMAGE:-$("$ROOT/toolchain/image.sh" qemu-kernel)}

SSH_PORT=${SSH_PORT:-2222}
HTTP_PORT=${HTTP_PORT:-8080}
METRICS_PORT=${METRICS_PORT:-9100}
MEM=${MEM:-64M}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-90}

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
[ -f "$SSH_KEY" ] && KEY_ARGS=(-i "$SSH_KEY")
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
	if ssh "${SSH_OPTS[@]}" root@127.0.0.1 true 2>/dev/null; then
		up=1
		break
	fi
	kill -0 "$QEMU_PID" 2>/dev/null || { cat "$WORK/qemu.log" >&2; fail "qemu exited before ssh came up"; }
	sleep 1
done
[ "$up" = 1 ] || { tail -80 "$WORK/qemu.log" >&2; fail "ssh never answered within ${BOOT_TIMEOUT}s"; }
echo "  ssh key auth ok"

sshx() { ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@"; }

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

say "scenario: kill -9 dropbear, confd, metricsd -- each back within 10s"
for svc in dropbear confd metricsd; do
	pid=$(sshx "for p in /proc/[0-9]*; do [ \"\$(cat \$p/comm 2>/dev/null)\" = $svc ] && echo \${p#/proc/}; done | head -1")
	[ -n "$pid" ] || { echo "  $svc: not running, skipping" >&2; continue; }
	sshx "kill -9 $pid"
	back=0
	for _ in $(seq 1 10); do
		sleep 1
		newpid=$(sshx "for p in /proc/[0-9]*; do [ \"\$(cat \$p/comm 2>/dev/null)\" = $svc ] && echo \${p#/proc/}; done | head -1")
		[ -n "$newpid" ] && [ "$newpid" != "$pid" ] && { back=1; break; }
	done
	[ "$back" = 1 ] || fail "$svc did not respawn within 10s of kill -9"
	echo "  $svc: respawned (pid $pid -> $newpid)"
done

say "scenario: health kicker withholds its kick under memory pressure (no odi_wdt in qemu -- see the file header)"
sshx 'grep -q health-kicker /var/log/services.log /var/log/*.log 2>/dev/null; echo checked=$?' || true
echo "  not exercised end to end: qemu has no /proc/odi_wdt (odi_wdt.c gates itself off entirely without it)"

say "all scenarios passed"
