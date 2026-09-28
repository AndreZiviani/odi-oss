#!/usr/bin/env bash
#
# CycloneDX 1.5 SBOM for a release image: every component that ends up on
# the flashed rootfs (kernel, the uClibc-ng runtime it and the packages
# below link against, the userland packages, our own daemons, and the two
# separate-repo binaries fetched by src/fetch-releases.sh), with version
# and license. Versions come from the pins already in this tree -- this
# script reads them, it does not carry its own copy -- so a bumped pin
# (kernel/618/fetch.sh, packages/*/build.sh, toolchain/images.env) shows up
# here without a second edit. docs/LICENSING.md is the prose version of the
# same table; keep both in sync by hand if a component license changes.
#
# Usage: tools/generate-sbom.sh <version, e.g. v1.0.4> <out/bin/releases.env, optional> > sbom.json
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${1:?usage: generate-sbom.sh <version> [releases.env]}
RELEASES_ENV=${2:-$ROOT/out/bin/releases.env}

grep1() { grep -m1 -oE "$2" "$1" || true; }

kernel_ver=$(grep1 "$ROOT/kernel/618/fetch.sh" 'VERSION=[0-9.]+' | cut -d= -f2)
busybox_ver=$(grep1 "$ROOT/packages/busybox/build.sh" 'VERSION=[0-9.]+' | cut -d= -f2)
dropbear_ver=$(grep1 "$ROOT/packages/dropbear/build.sh" 'VERSION=[0-9.]+' | cut -d= -f2)
iproute2_ver=$(grep1 "$ROOT/packages/iproute2/build.sh" 'VERSION=[0-9.]+' | cut -d= -f2)

# "# uClibc-ng toolchain (binutils 2.47, gcc 16.2.0, uClibc-ng 1.0.59, /opt/oss)"
toolchain_line=$(grep -m1 'uClibc-ng toolchain' "$ROOT/toolchain/images.env")
gcc_ver=$(echo "$toolchain_line" | grep -oE 'gcc [0-9.]+' | cut -d' ' -f2)
binutils_ver=$(echo "$toolchain_line" | grep -oE 'binutils [0-9.]+' | cut -d' ' -f2)
uclibc_ver=$(echo "$toolchain_line" | grep -oE 'uClibc-ng [0-9.]+' | cut -d' ' -f2)

# metricsd/confd: the tags actually fetched for this build if releases.env
# exists (src/fetch-releases.sh writes it into out/bin/), else the defaults
# fetch-releases.sh itself would use.
if [ -f "$RELEASES_ENV" ]; then
	# shellcheck disable=SC1090
	. "$RELEASES_ENV"
fi
metricsd_tag=${METRICSD_TAG:-$(grep1 "$ROOT/src/fetch-releases.sh" 'METRICSD_TAG:-v[0-9.]+' | grep -oE 'v[0-9.]+')}
confd_tag=${CONFD_TAG:-$(grep1 "$ROOT/src/fetch-releases.sh" 'CONFD_TAG-v[0-9.]+' | grep -oE 'v[0-9.]+')}

now=$(date -u +%Y-%m-%dT%H:%M:%SZ)

component() {
	name=$1 ver=$2 license=$3 supplier=$4 purl=$5
	printf '{"type":"library","name":"%s","version":"%s","licenses":[{"license":{"id":"%s"}}],"supplier":{"name":"%s"}%s}' \
		"$name" "$ver" "$license" "$supplier" \
		"$([ -n "$purl" ] && printf ',"purl":"%s"' "$purl" || true)"
}

{
	echo '{'
	echo '  "bomFormat": "CycloneDX",'
	echo '  "specVersion": "1.5",'
	echo '  "version": 1,'
	printf '  "metadata": {"timestamp": "%s", "component": {"type": "firmware", "name": "odi-oss", "version": "%s"}},\n' "$now" "$VERSION"
	echo '  "components": ['
	component "linux" "$kernel_ver" "GPL-2.0-only" "The Linux kernel community (kernel.org)" "pkg:generic/linux@$kernel_ver"
	echo ','
	component "gcc" "$gcc_ver" "GPL-3.0-with-GCC-exception" "GNU Project" ""
	echo ','
	component "binutils" "$binutils_ver" "GPL-3.0-only" "GNU Project" ""
	echo ','
	component "uClibc-ng" "$uclibc_ver" "LGPL-2.1-only" "uClibc-ng project" "pkg:generic/uclibc-ng@$uclibc_ver"
	echo ','
	component "busybox" "$busybox_ver" "GPL-2.0-only" "busybox.net" "pkg:generic/busybox@$busybox_ver"
	echo ','
	component "dropbear" "$dropbear_ver" "MIT" "Matt Johnston" "pkg:generic/dropbear@$dropbear_ver"
	echo ','
	component "iproute2" "$iproute2_ver" "GPL-2.0-only" "kernel.org" "pkg:generic/iproute2@$iproute2_ver"
	echo ','
	component "metricsd" "$metricsd_tag" "NOASSERTION" "AndreZiviani/odi-sfp-exporter" "pkg:github/AndreZiviani/odi-sfp-exporter@$metricsd_tag"
	echo ','
	component "confd" "${confd_tag:-local}" "NOASSERTION" "AndreZiviani/odi-ui" "pkg:github/AndreZiviani/odi-ui@${confd_tag:-local}"
	echo ','
	component "odi-oss-kernel-drivers" "$VERSION" "GPL-2.0-or-later" "AndreZiviani/odi-oss" "pkg:github/AndreZiviani/odi-oss@$VERSION"
	echo ','
	component "odi-oss-userland" "$VERSION" "GPL-2.0-or-later" "AndreZiviani/odi-oss" "pkg:github/AndreZiviani/odi-oss@$VERSION"
	echo '  ]'
	echo '}'
}
