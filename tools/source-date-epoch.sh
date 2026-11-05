#!/usr/bin/env sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Print the SOURCE_DATE_EPOCH for this tree: the committer date of HEAD, so
# any two checkouts of one commit agree and "now" never enters a build
# (https://reproducible-builds.org/docs/source-date-epoch/).
#
#     SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(tools/source-date-epoch.sh)}
#
# An already exported SOURCE_DATE_EPOCH wins, which is how tools/remote-build.sh
# hands the local value to a build host whose copy of the tree has no history.
# Without either, 0 (1970): still deterministic, and visibly not a real date.
set -eu
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
	echo "$SOURCE_DATE_EPOCH"
	exit 0
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 0
