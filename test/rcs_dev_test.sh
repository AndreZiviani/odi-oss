#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# The confirm-arp aid of rootfs/skeleton/etc/init.d/rcS.dev, off the device:
# the file holds the address to ARP, and an empty or invalid file means no
# target, so the boot confirms the watchdog as usual instead of looping on a
# reset. rcS.dev is sourced by /bin/sh with kmsg and arping stubbed; the
# config partition is a temporary directory (CONFIRM_ARP_FILE).
# shellcheck disable=SC2329 # the stubs are called by the sourced code
set -u
cd "$(dirname "$0")/.." || exit 1
D=$PWD/rootfs/skeleton/etc/init.d/rcS.dev
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; fail=$((fail + 1)); }

mkdir -p "$T/bin"
# arping answers only for the address in $T/answers; every call is logged.
cat > "$T/bin/arping" <<EOS
#!/bin/sh
for a; do last=\$a; done
echo "\$last" >> "$T/arping.log"
grep -qx "\$last" "$T/answers" 2>/dev/null
EOS
chmod +x "$T/bin/arping"

# run <file content, or "-" for no file>: prints "rc=<n> gw=<addr>"; stderr in $T/err, crumbs in $T/crumbs.
run() {
	rm -f "$T/confirm-arp" "$T/arping.log" "$T/err" "$T/crumbs"
	[ "$1" = "-" ] || printf '%b' "$1" > "$T/confirm-arp"
	PATH="$T/bin:$PATH" CONFIRM_ARP_FILE="$T/confirm-arp" CRUMBS="$T/crumbs" sh -c '
		kmsg() { :; }
		. "$0"
		crumb() { echo "$*" >> "$CRUMBS"; }
		dev_confirm_ok
		echo "rc=$? gw=$GW"
	' "$D" 2> "$T/err"
}
arpings() { [ -f "$T/arping.log" ] && cat "$T/arping.log" || true; }

echo 192.168.7.9 > "$T/answers"

out=$(run '192.168.7.9\n')
[ "$out" = "rc=0 gw=192.168.7.9" ] && ok "an address that answers confirms" || no "an address that answers confirms ($out)"
[ "$(arpings)" = 192.168.7.9 ] && ok "and that address was the one ARPed" || no "that address was the one ARPed ($(arpings))"

out=$(run '  10.0.0.5 \r\n')
[ "$out" = "rc=1 gw=10.0.0.5" ] && ok "surrounding whitespace and CR ignored; no answer does not confirm" || no "whitespace case ($out)"
[ "$(arpings)" = 10.0.0.5 ] && ok "and only the configured address was ARPed, never a derived .2" || no "only the configured address ARPed ($(arpings))"

for c in '' '\n' '   \n' 'not-an-ip\n' '192.168.7\n' '192.168.7.9.1\n' '192.168.7.256\n' '1..2.3\n' '.1.2.3\n' '1.2.3.\n' '1.2.3.4x\n' '1234.1.1.1\n'; do
	out=$(run "$c")
	label=$(printf '%s' "$c" | sed 's/\\n/<nl>/g')
	[ "$out" = "rc=0 gw=" ] && ok "no valid address ('$label'): confirms" || no "no valid address ('$label') confirms ($out)"
	[ -z "$(arpings)" ] && ok "  and nothing is ARPed" || no "  nothing ARPed ($(arpings))"
	grep -q "no valid IPv4 address" "$T/err" && grep -q "no valid IPv4 address" "$T/crumbs" \
		&& ok "  and it is logged to stderr and as a crumb" || no "  logged to stderr and crumb"
done

out=$(run -)
[ "$out" = "rc=0 gw=" ] && [ ! -s "$T/crumbs" ] && ok "no file: confirms, silent" || no "no file ($out)"

# The diagnostics skip, with a log line, when there is no configured address.
out=$(PATH="$T/bin:$PATH" CONFIRM_ARP_FILE="$T/none" sh -c '
	kmsg() { :; }
	. "$0"
	dev_klog_diag() { :; }
	dev_gw
	echo "gw=$GW"
' "$D")
[ "$out" = "gw=" ] && ok "no file: dev_gw leaves GW empty (no derived default host)" || no "dev_gw no default ($out)"

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
