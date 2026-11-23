#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

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
# NV_UNREAD=1 is our nv after a copy could not be read in full: the message on
# stdout, no value, exit 3 (src/nv/main.c).
# NV_RC=N is a read that gave no answer: nothing printed, exit N, as when the
# bound ends it (124, or 143 for busybox) or nv cannot run. NV_HANG=1 is an nv
# that never answers, for the bound itself. NV_UNREAD_TERSE=1 exits 3 with
# other words: the status is what counts, not the text. NV_VALUE_RC=N prints
# the value it was given and exits N all the same, so a value that came with
# a failed read can be told from one that came with exit 0.
# NV_FALLBACK_COMMIT is our nv answering for the fallback copy, and
# NV_FALLBACK_NONE=1 is our nv with no valid second copy.
# NV_OWN=1 is our nv as fwu.sh tells it: run with no arguments, it prints a
# usage that lists fallback. Without it the stub is the stock nv, whose usage
# (USAGE_RC, 1 by default; 22 on the stock base) has no fallback.
cat > "$T/nv" <<'STUB'
#!/bin/sh
if [ $# = 0 ]; then
	echo "Usage: nv getenv [parameter_name]"
	[ "${NV_OWN:-0}" = 1 ] && echo "       nv fallback [parameter_name]"
	exit 1
fi
[ -n "${NV_RC:-}" ] && exit "$NV_RC"
[ "${NV_HANG:-0}" = 1 ] && exec sleep 30
if [ "${NV_UNREAD:-0}" = 1 ]; then
	echo "nv: $1: a copy of the environment could not be read in full, so which one U-Boot uses is not known; run it again"
	exit 3
fi
[ "${NV_UNREAD_TERSE:-0}" = 1 ] && { echo "nv: read error"; exit 3; }
# NV_FALLBACK_UNREAD=1: the same, for the fallback read alone (getenv read
# both copies in full a moment before).
if [ "$1" = fallback ] && [ "${NV_FALLBACK_UNREAD:-0}" = 1 ]; then
	echo "nv: fallback: a copy of the environment could not be read in full, so which one U-Boot uses is not known; run it again"
	exit 3
fi
[ "$1" = fallback ] && [ -n "${NV_FALLBACK_RC:-}" ] && exit "$NV_FALLBACK_RC"
if [ "$1" = fallback ] && [ "${NV_FALLBACK_NONE:-0}" = 1 ]; then
	echo "nv: no valid second copy of the environment"
	exit 1
fi
if [ "$1" = fallback ] && [ -n "${NV_FALLBACK_COMMIT:-}" ]; then
	echo "sw_commit=$NV_FALLBACK_COMMIT"
	exit "${NV_FALLBACK_VALUE_RC:-0}"
fi
# The vendor nv has no fallback verb and prints its usage.
[ "$1" = getenv ] || { echo "Usage: nv getenv [parameter_name]"; exit "${USAGE_RC:-1}"; }
case "$2" in
sw_commit) [ -n "${NV_COMMIT:-}" ] && echo "sw_commit=$NV_COMMIT" ;;
sw_active) [ -n "${NV_ACTIVE:-}" ] && echo "sw_active=$NV_ACTIVE" ;;
esac
exit "${NV_VALUE_RC:-0}"
STUB
chmod +x "$T/nv"

# The nv is the stock one unless a run says NV_OWN=1.
run() {   # run <cmdline> <slot> [env...]
	cl=$1; slot=$2; shift 2
	echo "$cl" > "$T/cmdline"
	( cd "$T" && env PATH="$T:$PATH" PROC_MTD="$T/mtd" PROC_CMDLINE="$T/cmdline" \
	    "$@" \
	    sh "$OLDPWD/$FWU" "$slot" "$T/img.tar" 2>&1 )
	echo $? > "$T/rc"
}

check() {   # check <name> <expect-regex> <output>
	if printf '%s' "$3" | grep -q "$2"; then
		echo "ok    $1"; pass=$((pass + 1))
	else
		echo "FAIL  $1"; echo "        wanted /$2/, got:"; \
		printf '%s\n' "$3" | sed 's/^/        /'; fail=$((fail + 1))
	fi
}

# A refusal stops before the erase, with exit 1: the flash_eraseall check
# right after the guards, whose words are the sentinel, is never reached.
# There is no flash_eraseall on PATH here, so that check would also exit 1.
check_stops() {   # check_stops <name> <expect-regex> <output>
	check "$1" "$2" "$3"
	if printf '%s' "$3" | grep -q "flash_eraseall is not on this image"; then
		echo "FAIL  $1: went on past the guards"; fail=$((fail + 1))
	elif [ "$(cat "$T/rc")" != 1 ]; then
		echo "FAIL  $1: exit $(cat "$T/rc"), not 1"; fail=$((fail + 1))
	fi
}
# check_passes: the guards let it through to the flash_eraseall check.
check_passes() {   # check_passes <name> <output>
	check "$1" "flash_eraseall is not on this image" "$2"
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
# A copy of the environment that could not be read in full is not a missing
# sw_commit: the copy that was read may not be the one U-Boot boots by, so
# this refuses where a missing value only warns.
check "refuses when nv could not read a copy of the environment in full" \
      "is UNKNOWN. REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_UNREAD=1)"
check "FWU_FORCE=1 overrides that refusal, loudly" \
      "with an unknown revert target" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_UNREAD=1 FWU_FORCE=1)"
# The same when only the fallback read comes back incomplete: the vendor nv,
# which has no fallback verb, still only gets the warning.
check "refuses when nv could not read the fallback copy in full" \
      "fallback env copy in full.*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0 NV_FALLBACK_UNREAD=1 | tr '\n' ' ')"
check "FWU_FORCE=1 overrides that refusal too, loudly" \
      "with an unknown fallback" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0 NV_FALLBACK_UNREAD=1 FWU_FORCE=1)"
check "a vendor nv without a fallback verb is only warned about" \
      "Expected on the vendor nv" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0)"
# Exit 3 is the refusal whatever nv says with it.
check_stops "refuses on exit 3 with other words" \
      "is UNKNOWN. REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_UNREAD_TERSE=1)"
# A read that gave no answer is no answer: before the bound, fwu.sh waited
# for one; with it, it must not warn and write instead.
check_stops "refuses when nv getenv runs into the bound (coreutils, 124)" \
      "gave no answer (exit 124).*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_RC=124 | tr '\n' ' ')"
check_stops "refuses when nv getenv is killed by the bound (busybox, 143)" \
      "gave no answer (exit 143).*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_RC=143 | tr '\n' ' ')"
check_stops "refuses when nv fallback runs into the bound" \
      "fallback gave no answer (exit 124).*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0 NV_FALLBACK_RC=124 | tr '\n' ' ')"
check "FWU_FORCE=1 overrides the no-answer refusal, loudly" \
      "with an unknown revert target" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_RC=124 FWU_FORCE=1)"
# A value that came with a failed read is not taken, whatever it says.
check_stops "a sw_commit printed with exit 3 is not taken" \
      "is UNKNOWN. REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0 NV_VALUE_RC=3 | tr '\n' ' ')"
check_stops "a fallback sw_commit printed with exit 3 is not taken" \
      "fallback env copy in full.*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0 NV_FALLBACK_COMMIT=0 NV_FALLBACK_VALUE_RC=3 | tr '\n' ' ')"
check_stops "a sw_active printed with exit 3 is not taken" \
      "cannot tell which slot is running" \
      "$(run "console=ttyS0,115200 root=/dev/nowhere" 1 NV_ACTIVE=0 NV_COMMIT=0 NV_VALUE_RC=3 | tr '\n' ' ')"
check_stops "nor one printed by a read cut off by the bound" \
      "cannot tell which slot is running" \
      "$(run "console=ttyS0,115200 root=/dev/nowhere" 1 NV_ACTIVE=0 NV_COMMIT=0 NV_VALUE_RC=143 | tr '\n' ' ')"
check_passes "with exit 0 the same values pass" \
      "$(run "console=ttyS0,115200 root=/dev/nowhere" 1 NV_ACTIVE=0 NV_COMMIT=0 NV_FALLBACK_COMMIT=0)"
check "and the fallback copy is checked" \
      "the fallback env copy says 0 too -- ok" \
      "$(run "console=ttyS0,115200 root=/dev/nowhere" 1 NV_ACTIVE=0 NV_COMMIT=0 NV_FALLBACK_COMMIT=0)"
# Any other failed read: no value either, whatever it printed. Our nv fails
# only when it could not read, so there it refuses; how the vendor nv of a
# first flash fails is not known, so there it stays a warning.
OWN="NV_OWN=1"
check_stops "our nv: a failed getenv refuses" \
      "nv getenv failed (exit 1).*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_RC=1 | tr '\n' ' ')"
check_stops "our nv: a sw_commit printed with exit 2 is not taken" \
      "nv getenv failed (exit 2).*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_COMMIT=0 NV_VALUE_RC=2 | tr '\n' ' ')"
out=$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_COMMIT=0 NV_VALUE_RC=2 FWU_FORCE=1)
check "FWU_FORCE=1 overrides that refusal, loudly" "with an unknown revert target" "$out"
check_passes "and goes on past the guards" "$out"
out=$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0 NV_VALUE_RC=2)
check_passes "the vendor nv: a failed getenv is warned about" "$out"
check "and what it printed is not taken" \
      "exited 2; what it printed is not taken" "$out"
if printf '%s' "$out" | grep -q "revert target is intact"; then
	echo "FAIL  the vendor nv: a value from exit 2 read as intact"; fail=$((fail + 1))
else
	echo "ok    the vendor nv: a value from exit 2 is not read as intact"; pass=$((pass + 1))
fi
check_stops "a sw_active printed with exit 2 is not taken" \
      "cannot tell which slot is running" \
      "$(run "console=ttyS0,115200 root=/dev/nowhere" 1 NV_ACTIVE=0 NV_COMMIT=0 NV_VALUE_RC=2 | tr '\n' ' ')"
check_stops "our nv: a fallback sw_commit printed with exit 2 is not taken" \
      "nv fallback failed (exit 2).*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_COMMIT=0 NV_FALLBACK_COMMIT=0 NV_FALLBACK_VALUE_RC=2 | tr '\n' ' ')"
check_stops "our nv: a fallback that prints usage and exits 1 refuses" \
      "nv fallback failed (exit 1).*REFUSING" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_COMMIT=0 | tr '\n' ' ')"
out=$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_COMMIT=0 NV_FALLBACK_NONE=1)
check_passes "our nv: no valid second copy is not refused" "$out"
check "and says there is nothing to check" "no valid second env copy -- nothing to check" "$out"
out=$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_COMMIT=0 NV_FALLBACK_COMMIT=2)
check "a fallback sw_commit that is no slot is named as such" "says sw_commit=2, not a slot" "$out"
check_passes "our nv: values with exit 0 pass" \
      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 "$OWN" NV_COMMIT=0 NV_FALLBACK_COMMIT=0)"
# A slot built on the stock base: it is an image of this project, with
# /etc/odi-build, but the stock /bin/nv answers, and its fallback prints the
# usage and exits 22. That is the warning of the vendor nv, not a refusal.
out=$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_COMMIT=0 USAGE_RC=22)
check_passes "the stock nv on an ODI build: fallback exit 22 is not refused" "$out"
check "and the fallback is named as not checked" "was not readable -- NOT checked" "$out"

# The bound itself, where the host has timeout (macOS has none).
if command -v timeout >/dev/null 2>&1; then
	check_stops "an nv that hangs is cut off by the bound and refused" \
	      "gave no answer.*REFUSING" \
	      "$(run "console=ttyS0,115200 root=31:5 mtdparts=rtk_spi_nor_mtd:..." 1 NV_HANG=1 FWU_NV_TIMEOUT_S=1 | tr '\n' ' ')"
else
	echo "skip  the bound itself: no timeout on this host"
fi

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
