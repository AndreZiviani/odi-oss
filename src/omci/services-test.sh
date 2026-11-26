#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
# Actual omcli executable against the native dispatcher, no network or device.
set -eu
Q=qemu-mips-static
fail=0
. ./testlib.sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

run() {
	: > "$tmp/fixture"
	"$Q" respond/build/services_fixture "$1" > "$tmp/fixture" &
	pid=$!
	wait_for "$tmp/fixture" '^ready$' 5
	timeout 10 "$Q" cli/build/omcli services > "$tmp/$1"
	wait_bounded "$pid" 12 || return 1
}
run live
cat "$tmp/live"
cat > "$tmp/expected" <<'ROWS'
service id=1 state=live dir=3 kind=forward uni_mask=1 rule_gen=4 us_gem=20 ds_gem=20 tcont_me=? outer_action=pass inner_action=pass wan_outer=? wan_inner=37 pon_outer=? pon_inner=37 vlan_me=? gem_ctp_me=?
service id=2 state=live dir=3 kind=forward uni_mask=1 rule_gen=0 us_gem=20 ds_gem=20 tcont_me=? outer_action=pass inner_action=pass wan_outer=? wan_inner=? pon_outer=? pon_inner=? vlan_me=? gem_ctp_me=?
service id=4 state=pending dir=3 kind=forward uni_mask=1 rule_gen=4 us_gem=? ds_gem=? tcont_me=? outer_action=? inner_action=? wan_outer=? wan_inner=? pon_outer=? pon_inner=? vlan_me=? gem_ctp_me=?
service id=5 state=live dir=2 kind=forward uni_mask=1 rule_gen=7 us_gem=? ds_gem=20 tcont_me=? outer_action=pop inner_action=pass wan_outer=none wan_inner=tagged pon_outer=81 pon_inner=tagged vlan_me=? gem_ctp_me=?
summary live=3 pending=1 dry_run=0 rebuild_pending=0
ROWS
diff -u "$tmp/expected" "$tmp/live"
run dry
grep -q '^summary live=0 pending=1 dry_run=3 rebuild_pending=0$' "$tmp/dry"
run empty
printf '%s\n' 'summary live=0 pending=0 dry_run=0 rebuild_pending=0' > "$tmp/expected"
diff -u "$tmp/expected" "$tmp/empty"
echo 'ok    native omcli services (tagged, transparent, pending, dry-run, empty)'
