#!/usr/bin/env bash
#
# Release notes for a GitHub release: CHANGELOG.md's own section for the
# given version, plus a short flash-and-first-login paragraph. Used by
# .github/workflows/release.yml to build the release body; also runnable by
# hand -- tools/release-notes.sh v1.0.0.
#
# A pre-release tag (a hyphen after the version, v1.2.2-beta.1) has no
# CHANGELOG section of its own: its notes are the `## Unreleased` section,
# under a line saying it is a test build.
set -euo pipefail
VERSION=${1:?usage: release-notes.sh <version, e.g. v1.0.0> [CHANGELOG.md]}
CHANGELOG=${2:-$(dirname "$0")/../CHANGELOG.md}

[ -f "$CHANGELOG" ] || { echo "release-notes.sh: no such file: $CHANGELOG" >&2; exit 1; }

heading="## $VERSION "
case "$VERSION" in
*-*) heading="## Unreleased" ;;
esac

section=$(awk -v ver="$heading" '
	index($0 " ", ver) == 1 { found = 1; next }
	found && /^## / { exit }
	found { print }
' "$CHANGELOG")
[ -n "$section" ] ||
	{ echo "release-notes.sh: no CHANGELOG.md section starting \"$heading\"" >&2; exit 1; }

case "$VERSION" in
*-*)
	cat <<EOF
**Pre-release for testing.** Not a supported release: it carries the
changes merged since the last release, listed below as they stand in
\`CHANGELOG.md\` (\`## Unreleased\`). Trial-boot it and keep your current image
committed until it has proved itself on your line.

EOF
	;;
esac

# Trim leading and trailing blank lines from the extracted section.
printf '%s\n' "$section" | sed -e '/./,$!d' -e ':a' -e '/^\n*$/{$d;N;ba' -e '}'

cat <<EOF

## Flashing and first login

This is a keys-only image: root has no password at all, and ssh does not
offer a password prompt (\`ROOT_PW=locked\`, see \`docs/BUILDING.md\`). Flash
it into the slot you are not running and trial-boot it --
\`nv setenv sw_tryactive <slot>; reboot\` -- never commit on the first boot.
Full procedure, including how to read a boot you could not otherwise see:
[\`docs/FLASHING.md\`](docs/FLASHING.md).

First access is the web UI on port 80 (\`admin\`/\`admin\` until you change
it): add your SSH public key under its SSH-key admin page, then
\`ssh root@<stick>\` with that key -- it keeps working across reboots and
re-flashes. [\`docs/ACCESS.md\`](docs/ACCESS.md) has every way in and how to
move files on and off the stick.

Verify the download against \`SHA256SUMS\` before flashing anything:

    sha256sum -c SHA256SUMS
EOF
