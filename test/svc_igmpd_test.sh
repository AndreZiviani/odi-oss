#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/svc-igmpd.sh, off the device: the daemon runs
# only for IGMP_SNOOPING=1, and every other way out parks in respawn-off.sh
# (a bare exit would have init respawn the script in a tight loop). The
# script is copied with its absolute paths moved under a temporary root, and
# flash, respawn.sh and respawn-off.sh are stubs that say what they were
# asked to do.
set -u
cd "$(dirname "$0")/.." || exit 1
V=$PWD/rootfs/skeleton/etc/scripts/svc-igmpd.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; fail=$((fail + 1)); }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else no "$1" "wanted [$2], got [$3]"; fi; }

mkdir -p "$T/etc/scripts" "$T/etc/config" "$T/bin"
sed -e "s|/etc/|$T/etc/|g" -e "s|/bin/igmpd|$T/bin/igmpd|g" "$V" > "$T/svc.sh"
cat > "$T/etc/scripts/respawn-off.sh" <<'EOF'
#!/bin/sh
echo OFF
EOF
cat > "$T/etc/scripts/respawn.sh" <<'EOF'
#!/bin/sh
echo "RUN $*"
EOF
cat > "$T/etc/scripts/flash" <<'EOF'
#!/bin/sh
# flash get KEY: KEY=value, as the real one prints it
case "$2" in
IGMP_PON_DROP) echo "$2=${FAKE_DROP-}" ;;
*) echo "$2=${FAKE_VALUE-}" ;;
esac
EOF
chmod +x "$T"/etc/scripts/*
: > "$T/bin/igmpd"; chmod +x "$T/bin/igmpd"

run() { FAKE_VALUE="$1" FAKE_DROP="${2-}" sh "$T/svc.sh" 2>&1; }

eq "IGMP_SNOOPING=1 runs igmpd, writing, under respawn.sh" \
   "RUN 0 /var/log/services.log $T/bin/igmpd -w" "$(run 1)"
# IGMP_PON_DROP: strictly opt-in; only 1 asks for the drop.
eq "IGMP_PON_DROP=0 snoops without the drop" \
   "RUN 0 /var/log/services.log $T/bin/igmpd -w" "$(run 1 0)"
eq "IGMP_PON_DROP=1 drops" \
   "RUN 0 /var/log/services.log $T/bin/igmpd -w -d" "$(run 1 1)"
eq "IGMP_PON_DROP=yes is read as off" \
   "RUN 0 /var/log/services.log $T/bin/igmpd -w" "$(run 1 yes)"
eq "IGMP_PON_DROP=1 alone does not start the daemon" OFF "$(run "" 1)"
eq "unset parks" OFF "$(run "")"
eq "0 parks" OFF "$(run 0)"
eq "anything but 1 parks" OFF "$(run yes)"
: > "$T/etc/config/igmpd.off"
eq "igmpd.off parks even when set" OFF "$(run 1)"
rm -f "$T/etc/config/igmpd.off"
: > "$T/etc/config/modules.off"
eq "modules.off parks" OFF "$(run 1)"
rm -f "$T/etc/config/modules.off"
rm -f "$T/bin/igmpd"
eq "no binary parks" OFF "$(run 1)"

# The gate is a real key: flash knows it, and init has the entry.
grep -q 'IGMP_SNOOPING' rootfs/skeleton/etc/scripts/flash && ok "flash lists IGMP_SNOOPING as an odi-only key" || no "flash lists IGMP_SNOOPING"
grep -q 'IGMP_PON_DROP' rootfs/skeleton/etc/scripts/flash && ok "flash lists IGMP_PON_DROP as an odi-only key" || no "flash lists IGMP_PON_DROP"
grep -q '^::respawn:/etc/scripts/svc-igmpd.sh$' rootfs/skeleton/etc/inittab && ok "inittab has the respawn entry" || no "inittab has the respawn entry"

echo "$pass ok, $fail failed"
[ "$fail" -eq 0 ]
