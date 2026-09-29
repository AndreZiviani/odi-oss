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
# What an interactive login prints: dropbear shows /etc/motd only for a
# login shell on a pty, never for `ssh host cmd`, so this asks for a shell
# and feeds it exit.
login_banner() {
	printf 'exit\n' | timeout 20 ssh -tt "${SSH_OPTS[@]}" root@127.0.0.1 2>/dev/null | tr -d '\r' || true
}

say "scenario: slot-state.sh, the inittab once entry, wrote the slot state at boot"
# The malta kernel boots with no root=31:N and no U-Boot environment: the
# state file must still exist, with the running slot and uncommitted left
# empty (unknown, never a guessed 0), the motd must say so, and dropbear
# must print that motd on an interactive login -- the native path the
# TRIAL BOOT notice takes on a stick.
ok=0
for _ in $(seq 1 20); do
	sshx 'grep -qx "uncommitted=" /var/run/odi-slot' 2>/dev/null && { ok=1; break; }
	sleep 1
done
[ "$ok" = 1 ] || { sshx 'cat /var/run/odi-slot' >&2 || true; fail "slot-state.sh wrote no /var/run/odi-slot with uncommitted= at boot"; }
sshx 'grep -qx "running=" /var/run/odi-slot' || fail "odi-slot names a running slot qemu does not have"
sshx 'grep -q "SLOT STATE UNKNOWN" /etc/motd' || fail "/etc/motd does not carry the unknown-state notice"
banner=$(login_banner)
echo "$banner" | grep -q "SLOT STATE UNKNOWN" || fail "dropbear did not print /etc/motd at an interactive login"
echo "  /var/run/odi-slot written (state unknown under qemu), motd printed at login"

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

say "syslogd and klogd run, and logread carries dropbear logins"
# Every ssh call above is a dropbear login, which dropbear now logs through
# syslog (no -E, svc-dropbear.sh) rather than to services.log.
sshx 'pidof syslogd >/dev/null' || fail "syslogd is not running"
sshx 'pidof klogd >/dev/null' || fail "klogd is not running"
n=$(sshx 'logread | grep -c dropbear' 2>/dev/null) || n=0
[ "$n" -gt 0 ] || fail "logread has no dropbear lines"
echo "  syslogd, klogd up; logread has $n dropbear lines"

say "event lines reach syslog: the kernel through klogd, omcid through /dev/log"
# The odi_gpon event=onu_state lines are printk (a stock malta kernel has no
# odi_gpon to print them), so the kernel path is checked with a line of our
# own through /dev/kmsg: printk -> klogd -> syslogd, the path they take. A
# line written from userspace keeps facility user (the kernel does not let
# /dev/kmsg claim kern), so only the tag and the priority are matched.
sshx 'echo "<5>odi-test: event=klogd_path" > /dev/kmsg' || true
n=0
for _ in 1 2 3 4 5; do
	n=$(sshx 'logread | grep -c "[a-z]*\.notice kernel: .*odi-test: event=klogd_path"' 2>/dev/null) || n=0
	[ "$n" -gt 0 ] && break
	sleep 1
done
[ "$n" -gt 0 ] || fail "a kernel message did not reach logread through klogd"
# omcid's start line, from the respawn scenario above, and a CLI MIB reset:
# both straight to /dev/log (src/omci/respond/events.c), facility daemon.
n=$(sshx 'logread | grep -c "daemon.[a-z]* omcid\[[0-9]*\]: event=start "' 2>/dev/null) || n=0
[ "$n" -gt 0 ] || fail "logread has no omcid event=start line"
sshx '/bin/omcli -f mib reset' >/dev/null 2>&1 || true
n=0
for _ in 1 2 3 4 5; do
	n=$(sshx 'logread | grep -c "daemon.notice omcid\[[0-9]*\]: event=mib_reset side=local "' 2>/dev/null) || n=0
	[ "$n" -gt 0 ] && break
	sleep 1
done
[ "$n" -gt 0 ] || fail "logread has no omcid event=mib_reset line"
echo "  kernel and omcid event lines both in logread"

say "scenario: NTP_SERVER syncs the guest clock (opt-in, skips if no host responder)"
# The guest config fixture (build-initramfs.sh) sets NTP_SERVER=10.0.2.2,
# the qemu user-net gateway address -- SLIRP maps it straight to the host
# own loopback, so any NTP server listening there answers svc-ntpd.sh in
# the guest without extra guestfwd wiring. An NTP server the build host
# already runs on UDP 123 is used as it is (it also holds the port, so
# nothing else could bind it); otherwise a host busybox that has the ntpd
# applet serves with -l for the length of this scenario. Neither: skipped,
# not failed -- this harness does not hand-roll a fake NTP protocol server.
NTP_HOST_PID=""
NTP_HOST_LOG="$WORK/host-ntpd.log"
NTP_READY=""
if ss -uln 2>/dev/null | awk '{print $4}' | grep -qE '^(127\.0\.0\.1|0\.0\.0\.0|\*):123$'; then
	NTP_READY="the NTP server this host already runs"
elif command -v busybox >/dev/null 2>&1 && busybox --list 2>/dev/null | grep -qx ntpd; then
	busybox ntpd -n -l >"$NTP_HOST_LOG" 2>&1 &
	NTP_HOST_PID=$!
	sleep 1
	kill -0 "$NTP_HOST_PID" 2>/dev/null && NTP_READY="host busybox ntpd -l (pid $NTP_HOST_PID)"
fi
if [ -n "$NTP_READY" ]; then
	echo "  responder: $NTP_READY, reached from the guest at 10.0.2.2:123"
	# A deliberately wrong guest clock, far enough off that the first
	# ntpd correction steps rather than slews -- proves ntpd set the
	# clock, not that it was already close. Then apply.sh ntp restarts
	# ntpd, so it polls at once instead of at whatever long interval it
	# had backed off to since boot (and the SERVICE RESTART verb gets
	# exercised on the way).
	sshx "date -u -s '2000-01-01 00:00:00'" >/dev/null
	before=$(sshx date -u +%s)
	sshx "/etc/scripts/apply.sh ntp"
	synced=0
	for _ in $(seq 1 60); do
		sleep 1
		now=$(sshx date -u +%s) || continue
		host_now=$(date -u +%s)
		diff=$((now > host_now ? now - host_now : host_now - now))
		[ "$diff" -lt 10 ] && { synced=1; break; }
	done
	[ -z "$NTP_HOST_PID" ] || kill "$NTP_HOST_PID" 2>/dev/null || true
	if [ "$synced" = 1 ]; then
		echo "  guest clock corrected: was $before, now within 10s of host wall time"
	else
		cat "$NTP_HOST_LOG" >&2 2>/dev/null || true
		sshx "logread | grep -i ntpd | tail -20" >&2 || true
		fail "guest clock did not sync against NTP_SERVER within 60s"
	fi
else
	echo "  no NTP server on this host and no busybox ntpd applet -- skipping (see docs/HACKING.md, test-qemu)"
fi

say "scenario: a web UI save of SYSLOG_SERVER is read back by svc-syslogd.sh"
# The path a stick takes: confd runs the REAL /etc/scripts/flash set against the
# writable config dir, then /api/apply restarts syslogd under init respawn and
# svc-syslogd.sh reads the value back through flash get. SYSLOG_SERVER is an
# odi-only key (not in the stock XML), which is what once could not be saved.
syslogd_args() {
	# shellcheck disable=SC2016
	sshx 'for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = syslogd ] && tr "\0" " " < $p/cmdline; done' 2>/dev/null || true
}
UIAUTH=admin:admin
res=$(curl -s -u "$UIAUTH" -X POST --data 'SYSLOG_SERVER=10.0.2.2:5514' "http://127.0.0.1:$HTTP_PORT/api/config") || res=""
case "$res" in *'"ok":true'*) ;; *) fail "web UI save of SYSLOG_SERVER failed: $res" ;; esac
[ "$(sshx 'grep -c "^SYSLOG_SERVER=10.0.2.2:5514$" /etc/config/odi.conf')" = 1 ] || fail "odi.conf does not hold SYSLOG_SERVER"
sshx '/etc/scripts/flash get SYSLOG_SERVER' | grep -qx 'SYSLOG_SERVER=10.0.2.2:5514' || fail "flash get SYSLOG_SERVER does not read the saved value"
sshx 'grep -q "Name=\"LAN_IP_ADDR\" Value=\"10.0.2.15\"" /etc/config/lastgood.xml' || fail "a stock key in lastgood.xml was touched"
curl -s -u "$UIAUTH" -X POST --data 'what=syslog' "http://127.0.0.1:$HTTP_PORT/api/apply" >/dev/null || true
ok=0
for _ in $(seq 1 15); do
	syslogd_args | grep -q -- '-R 10.0.2.2:5514' && { ok=1; break; }
	sleep 1
done
[ "$ok" = 1 ] || fail "syslogd is not running with -R 10.0.2.2:5514 after apply (args: $(syslogd_args))"
echo "  saved through the UI, stored in odi.conf, syslogd running with -R 10.0.2.2:5514"
# Clearing: an empty value removes the key, and syslogd comes back without -R.
sshx '/etc/scripts/flash set SYSLOG_SERVER ""' >/dev/null
sshx '/etc/scripts/apply.sh syslog' >/dev/null || true
ok=0
for _ in $(seq 1 15); do
	a=$(syslogd_args)
	[ -n "$a" ] && ! echo "$a" | grep -q -- '-R ' && { ok=1; break; }
	sleep 1
done
[ "$ok" = 1 ] || fail "syslogd still forwards after the key was cleared (args: $(syslogd_args))"
echo "  cleared: syslogd back to local only"

say "scenario: slot-state.sh on a trial (fixtures, real busybox)"
# The same script under the real busybox ash, sed and timeout, against a stub
# nv printing the two copies in the real format: slot 1 running, both copies
# still committing slot 0. test/slot_state_test.sh has the other shapes.
sshx 'sh -s' <<'GUEST'
set -e
A=/tmp/ss; rm -rf $A; mkdir -p $A
echo "console=ttyS0 root=31:7" > $A/cmdline
printf 'mtd5: 00300000 00001000 "r0"\nmtd7: 00300000 00001000 "r1"\n' > $A/mtd
cat > $A/nv <<'NV'
#!/bin/sh
case "$1" in
getenv)   printf 'Valid environment: 2\nsw_active=1\nsw_commit=0\nsw_tryactive=2\n\n' ;;
fallback) printf 'Fallback environment: 1\nsw_active=0\nsw_commit=0\nsw_tryactive=1\n\n' ;;
*) exit 1 ;;
esac
NV
chmod +x $A/nv
NV=$A/nv PROC_CMDLINE=$A/cmdline PROC_MTD=$A/mtd /etc/scripts/slot-state.sh
GUEST
sshx 'grep -qx "uncommitted=1" /var/run/odi-slot && grep -qx "running=1" /var/run/odi-slot && grep -qx "next_boot=0" /var/run/odi-slot' || \
	{ sshx 'cat /var/run/odi-slot' >&2 || true; fail "odi-slot does not record the trial"; }
banner=$(login_banner)
echo "$banner" | grep -q "TRIAL BOOT: running slot 1, which is not committed (sw_commit=0 in both copies)" || \
	fail "the login banner does not carry the TRIAL BOOT notice (got: $banner)"
sshx 'logread | grep -q "slot-state: TRIAL BOOT: running slot 1"' || fail "the TRIAL BOOT notice did not reach syslog"
sshx 'rm -rf /tmp/ss'
echo "  odi-slot records uncommitted=1, the login banner and syslog carry the TRIAL BOOT notice"

say "all scenarios passed"
