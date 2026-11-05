#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# CycloneDX 1.5 SBOM for a release image, on stdout. The work is in
# tools/sbom.py (its header says what the SBOM contains and why it is python3
# and not shell); this wrapper keeps the command the release job and the docs
# name.
#
# Usage: tools/generate-sbom.sh <version, e.g. v1.0.4> [out/bin/releases.env] > sbom.cdx.json
#
#   SBOM_STRICT=1   fail when an artifact that should carry a hash is missing
#                   (the release job sets it); otherwise it is only a warning
#   SBOM_IMAGE=f    the image tarball, hashed into metadata.component
#   BIN_DIR, ASSET_DIR, PKG_DIR, BUSYBOX, KERNEL_IMAGE
#                   where the build left its files, as in image/build.sh
#
# Check the result with tools/verify-sbom.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${1:?usage: generate-sbom.sh <version> [releases.env]}
args=("$VERSION")
[ -z "${2:-}" ] || args+=(--releases-env "$2")
[ "${SBOM_STRICT:-0}" != 1 ] || args+=(--strict)
[ -z "${SBOM_IMAGE:-}" ] || args+=(--image "$SBOM_IMAGE")
exec python3 -I "$ROOT/tools/sbom.py" generate "${args[@]}"
