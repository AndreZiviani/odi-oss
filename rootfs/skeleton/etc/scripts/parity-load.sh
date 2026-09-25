#!/bin/sh
# Read a v6-parity load table (tools/regdump/mkparity.py's own output
# format: "offset value mask name" per line, hex, no 0x prefix, comments
# starting with # and blank lines both skipped) and feed it to
# odi_switch_init_parity() through /proc/odi_omci -- one `parity_add
# <offset> <value> <mask>` write per entry, then one bare `init_parity`
# write to run every entry just loaded ("parity table from a file").
#
#     parity-load.sh [--dry-run] <table-file> [/proc/odi_omci path]
#
# --dry-run prints the parity_add/init_parity lines this script would write
# instead of writing them -- how test/parity_load_test.sh exercises this
# parsing logic on the host, without a kernel or a stick. Without it, rcS
# (rootfs/skeleton/etc/init.d/rcS) calls this against the real
# /proc/odi_omci right after the init_platform trigger.
#
# At most 64 entries are read (ODI_SWITCH_PARITY_MAX_LOADED,
# odi_switch_dal.h) -- the same cap odi_switch_parity_add() itself enforces
# on the kernel side (a 65th accepted entry there would just silently
# never run past the loaded array), applied here too so a table file with
# too many entries gets one clear warning instead of a truncated load
# nobody was told about.
set -eu

MAX_ENTRIES=64

dry_run=0
if [ "${1:-}" = "--dry-run" ]; then
	dry_run=1
	shift
fi

TABLE=${1:?usage: parity-load.sh [--dry-run] <table-file> [/proc/odi_omci path]}
PROC=${2:-/proc/odi_omci}

emit() {
	if [ "$dry_run" -eq 1 ]; then
		echo "$1"
	else
		echo "$1" > "$PROC"
	fi
}

n=0
while read -r offset value mask name; do
	case $offset in
	""|\#*) continue ;;
	esac
	if [ -z "${value:-}" ] || [ -z "${mask:-}" ]; then
		echo "parity-load.sh: malformed line, skipped: $offset $value $mask $name" >&2
		continue
	fi
	if [ "$n" -ge "$MAX_ENTRIES" ]; then
		echo "parity-load.sh: $TABLE has more than $MAX_ENTRIES entries, the rest are refused" >&2
		break
	fi
	emit "parity_add $offset $value $mask"
	n=$((n + 1))
done < "$TABLE"

if [ "$n" -eq 0 ]; then
	echo "parity-load.sh: no valid entries in $TABLE, nothing loaded" >&2
	exit 0
fi

emit "init_parity"
echo "parity-load.sh: loaded $n entries from $TABLE, init_parity triggered" >&2
