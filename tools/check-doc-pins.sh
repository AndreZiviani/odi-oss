#!/usr/bin/env bash
#
# Docs that name a pinned version must name the pinned one. The kernel
# version is read from VERSION= in kernel/618/fetch.sh; every 6.18.N in
# README.md and docs/BUILDING.md has to equal it. docs/BUILDING.md also names
# the confd and metricsd tags, checked against src/fetch-releases.sh.
# Run by `make lint`; run from the repository root.
set -eu

status=0

kver=$(sed -n 's/^VERSION=\(6\.18\.[0-9][0-9]*\)$/\1/p' kernel/618/fetch.sh)
if [ -z "$kver" ]; then
	echo "check-doc-pins: no VERSION=6.18.N in kernel/618/fetch.sh" >&2
	exit 1
fi
for f in README.md docs/BUILDING.md; do
	bad=$(grep -noE '6\.18\.[0-9]+' "$f" | grep -v ":$kver\$" || true)
	if [ -n "$bad" ]; then
		echo "check-doc-pins: $f names a kernel other than the pinned $kver:" >&2
		echo "$bad" >&2
		status=1
	fi
done

confd=$(sed -n 's/^CONFD_TAG=\${CONFD_TAG-\(v[0-9.]*\)}$/\1/p' src/fetch-releases.sh)
metricsd=$(sed -n 's/^METRICSD_TAG=\${METRICSD_TAG:-\(v[0-9.]*\)}$/\1/p' src/fetch-releases.sh)
if [ -z "$confd" ] || [ -z "$metricsd" ]; then
	echo "check-doc-pins: cannot read the confd/metricsd tags from src/fetch-releases.sh" >&2
	exit 1
fi
if ! grep -qF "confd $confd, metricsd $metricsd" docs/BUILDING.md; then
	echo "check-doc-pins: docs/BUILDING.md does not say \"confd $confd, metricsd $metricsd\"" >&2
	status=1
fi

exit "$status"
