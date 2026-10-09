#!/usr/bin/env bash
#
# The refusals in image/fwu.sh, exercised off the device.
#
# Only the guards: everything past them erases flash. The point is that the one
# check that matters -- "is this the slot we are running from" -- is proven
# rather than asserted, which is exactly what the old comment did while the
# check itself did not exist.
#
# /proc/mtd and /proc/cmdline are real files on the stick and variables here,
# and the fixtures are the verbatim captures from the reference sticks.
set -u
cd "$(dirname "$0")/.." || exit 1
FWU=image/fwu.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

cat > "$T/mtd" <<'EOF'
dev:    size   erasesize  name
mtd0: 00040000 00001000 "boot"
mtd1: 00002000 00001000 "env"
mtd2: 00002000 00001000 "env2"
mtd3: 0003c000 00001000 "config"
mtd4: 0014c000 00001000 "k0"
mtd5: 00274000 00001000 "r0"
mtd6: 0014c000 00001000 "k1"
mtd7: 00274000 00001000 "r1"
EOF
# A tarball and an md5.txt only have to exist to get past the early checks.
: > "$T/md5.txt"
: > "$T/img.tar"

# A stub nv, so the sw_commit branch is reachable at all. fwu.sh calls
# `nv getenv sw_commit` unqualified, so putting this first on PATH is enough.
# NV_COMMIT unset means nv prints nothing, which is the cannot-read case.
cat > "$T/nv" <<'STUB'
#!/bin/sh
[ "$1" = getenv ] || exit 1
case "$2" in
sw_commit) [ -n "${NV_COMMIT:-}" ] && echo "sw_commit=$NV_COMMIT" ;;
sw_active) [ -n "${NV_ACTIVE:-}" ] && echo "sw_active=$NV_ACTIVE" ;;
esac
exit 0
STUB
chmod +x "$T/nv"

run() {   # run <cmdline> <slot> [env...]
	cl=$1; slot=$2; shift 2
	echo "$cl" > "$T/cmdline"
	( cd "$T" && env PATH="$T:$PATH" PROC_MTD="$T/mtd" PROC_CMDLINE="$T/cmdline" "$@" \
	    sh "$OLDPWD/$FWU" "$slot" "$T/img.tar" 2>&1 )
}

check() {   # check <name> <expect-regex> <output>
	if printf '%s' "$3" | grep -q "$2"; then
		echo "ok    $1"; pass=$((pass + 1))
	else
		echo "FAIL  $1"; echo "        wanted /$2/, got:"; \
		printf '%s\n' "$3" | sed 's/^/        /'; fail=$((fail + 1))
	fi
}

# isp1 runs slot 0 (root=31:5 = mtd5 = r0), isp2 runs slot 1 (31:7 = r1).
check "refuses the running slot (isp1, slot 0)" \
      "RUNNING FROM" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 0)"
check "refuses the running slot (isp2, slot 1)" \
      "RUNNING FROM" \
      "$(run "console=ttyS0,115200 root=31:7 mtdparts=rtk_spi_nor_mtd:..." 1)"
check "allows the idle slot (isp1 flashing 1)" \
      "running slot is 0, target is 1 -- ok" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1)"
check "allows the idle slot (isp2 flashing 0)" \
      "running slot is 1, target is 0 -- ok" \
      "$(run "console=ttyS0,115200 root=31:7 mtdparts=rtk_spi_nor_mtd:..." 0)"
# The real slot-1 command line: the built-in slot-0 line first, then
# U-Boot's arguments; the kernel mounts the last root= (issue #42).
DUP="console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:... print-fatal-signals=1 console=ttyS0,115200 mtdparts=rtk_spi_nor_mtd:... root=31:7"
check "two root=: refuses the running slot, the last one (slot 1)" \
      "RUNNING FROM" \
      "$(run "$DUP" 1)"
check "two root=: allows slot 0 while slot 1 runs" \
      "running slot is 1, target is 0 -- ok" \
      "$(run "$DUP" 0)"
# The revert target. Refusing the running slot protects the write; sw_commit
# is what protects the box afterwards, and nothing checked it until now.
check "refuses when sw_commit names the slot being written" \
      "sw_commit is 1, the slot being written" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=1)"
check "accepts when sw_commit names the running slot" \
      "revert target is intact" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0)"
check "says so when sw_commit cannot be read" \
      "revert target is UNKNOWN" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1)"
check "FWU_FORCE=1 overrides the sw_commit refusal, loudly" \
      "no good revert target" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=1 FWU_FORCE=1)"

check "refuses when the running slot cannot be determined" \
      "REFUSING" \
      "$(run "console=ttyS0,115200 root=/dev/nowhere" 0)"
check "FWU_FORCE=1 overrides only the unknown case" \
      "proceeding without knowing" \
      "$(run "console=ttyS0,115200 root=/dev/nowhere" 0 FWU_FORCE=1)"
check "FWU_FORCE=1 does NOT override a known running slot" \
      "RUNNING FROM" \
      "$(run "console=ttyS0,115200 root=31:5" 0 FWU_FORCE=1)"
check "rejects a slot that is not 0 or 1" \
      "usage:" \
      "$(run "console=ttyS0,115200 root=31:5" 2)"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
