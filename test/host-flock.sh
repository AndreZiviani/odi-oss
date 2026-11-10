# shellcheck shell=sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by the host tests that run /etc/scripts/flash, from the repo root.
# flash needs flock (a busybox applet on the stick); a host without one gets
# test/bin/flock, which takes the same kernel lock through python.
command -v flock >/dev/null 2>&1 || PATH="$PWD/test/bin:$PATH"
export PATH
