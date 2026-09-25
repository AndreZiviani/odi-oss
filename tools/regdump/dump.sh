#!/bin/sh
# Read every address in a regdump list, in order, one memprobe reg call per
# address, nothing else. Runs ON THE STICK -- busybox ash only, so it uses
# nothing beyond sh builtins (read/while/case) and the two external tools
# named on the command line. No head, no tail, no od, no awk assumed present.
#
#     dump.sh <list-file> [memprobe-path]
#
# list-file is the output of mklist.py: comment lines start with "#", every
# other line is "<addr_hex> <name>" -- only the first field is read here,
# the name is bookkeeping mklist.py keeps for diff.py, not needed on the
# stick. memprobe-path defaults to /tmp/memprobe.
#
# Output is exactly one line per address, in list-file order, in the "reg"
# format memprobe already prints -- this script does not reformat it:
#
#     <addr_hex> = <value_hex>
#
# Nothing else goes to stdout: no progress, no summary, no blank lines.
# Redirect the whole run to a file and pull that file, unchanged, for
# diff.py. A pause here (a prompt, a sleep, a retry) would leave the two
# sides of the A/B captured at different boot moments, so this script
# pauses for nothing and does not retry a failed read -- if memprobe itself
# hangs the whole run hangs, which is a memprobe/list problem to fix, not
# something for this script to paper over.
set -eu

LIST=${1:?usage: dump.sh <list-file> [memprobe-path]}
PROBE=${2:-/tmp/memprobe}

while read -r addr _name; do
	case $addr in
	""|\#*) continue ;;
	esac
	"$PROBE" reg "$addr"
done < "$LIST"
