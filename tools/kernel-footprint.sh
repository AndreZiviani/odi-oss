#!/usr/bin/env bash
#
# How much of the mainline tree our kernel patches edit.
#
#     tools/kernel-footprint.sh [patch...]    default: kernel/618/patches/*.patch
#
# Moving to the next longterm kernel costs little for a file we add (it is
# copied across) and a lot for every line we change in a file mainline
# already has (it has to be re-applied, and re-read, against new code). This
# counts the second kind: the + and - lines of every hunk against a file
# that exists in the pristine tree, per patch and in total, and the number
# of distinct mainline files touched. Hunks against files a patch creates
# (and later edits to them) are not counted; kernel/extra is reported
# separately as the overlay size.
#
# A file counts as ours when a patch creates it (a first hunk of -0,0), so
# no kernel tree is needed. Hunks are consumed by their line counts, not by
# the first character alone, so a removed line that itself starts with
# dashes is not mistaken for a file header.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ $# -eq 0 ]; then
	set -- "$ROOT"/kernel/618/patches/*.patch
fi

awk '
function flush_patch() {
	if (patch != "") {
		nf = 0
		for (k in pfiles) nf++
		printf "%-44s %5d lines %3d files\n", patch, plines, nf
		delete pfiles
	}
}
FNR == 1 { flush_patch(); patch = FILENAME; sub(/.*\//, "", patch); plines = 0; old = 0; new = 0 }
# Inside a hunk: consume exactly the lines its header announced.
old > 0 || new > 0 {
	c = substr($0, 1, 1)
	if (c == "-") { old--; if (!ours[f]) { plines++; total++; pfiles[f] = 1; tfiles[f] = 1 } }
	else if (c == "+") { new--; if (!ours[f]) { plines++; total++; pfiles[f] = 1; tfiles[f] = 1 } }
	else if (c == "\\") { }
	else { old--; new-- }
	next
}
/^\+\+\+ / { f = $2; sub(/^[^\/]*\//, "", f); first = 1; next }
/^@@ / {
	split($2, a, ","); split($3, b, ",")
	old = (a[2] == "") ? 1 : a[2] + 0
	new = (b[2] == "") ? 1 : b[2] + 0
	if (first && a[1] == "-0" && old == 0) ours[f] = 1
	first = 0
	next
}
END {
	flush_patch()
	nt = 0
	for (k in tfiles) nt++
	printf "%-44s %5d lines %3d files\n", "total (mainline files)", total, nt
}' "$@"

extra="$ROOT/kernel/extra"
if [ -d "$extra" ]; then
	n=$(find "$extra" -type f ! -name README.md | wc -l)
	l=$(find "$extra" -type f ! -name README.md -exec cat {} + | wc -l)
	printf "%-44s %5d lines %3d files\n" "overlay (kernel/extra, ours)" "$l" "$n"
fi
