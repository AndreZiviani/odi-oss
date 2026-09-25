#!/usr/bin/env bash
#
# rootfs/skeleton/etc/scripts/network.sh addr: the addresses re-applied live,
# off the device.
#
# ifconfig is a stub that keeps one file per interface or alias under $T/if,
# holding "ADDR MASK", and prints it back in busybox ifconfig format -- the
# only two things addr reads and writes. What is checked: the primary moves
# only when the store says something else, the second address follows
# LAN_ENABLE_IP2 both ways, the dry run changes nothing, and /etc/config/lan-ip
# still wins over LAN_IP_ADDR.
set -u
cd "$(dirname "$0")/.." || exit 1
N=$PWD/rootfs/skeleton/etc/scripts/network.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
t() {
	if [ "$2" = "$3" ]; then echo "ok    $1"; pass=$((pass + 1))
	else echo "FAIL  $1"; echo "        want [$2]"; echo "        got  [$3]"; fail=$((fail + 1)); fi
}

mkdir -p "$T/if" "$T/sys/br0"
cat > "$T/ifconfig" <<'STUB'
#!/bin/sh
f="$IFSTATE/$1"
case "${2:-}" in
"")   [ -f "$f" ] || { echo "ifconfig: $1: error fetching interface information" >&2; exit 1; }
      read -r a m < "$f"
      printf '%s  Link encap:Ethernet\n          inet addr:%s  Bcast:0.0.0.0  Mask:%s\n' "$1" "$a" "$m" ;;
down) rm -f "$f" ;;
*)    echo "$2 $4" > "$f"; echo "$*" >> "$IFSTATE/calls" ;;
esac
STUB
chmod +x "$T/ifconfig"
echo "192.168.0.1 255.255.255.0" > "$T/if/br0"

store() {   # store <LAN_IP_ADDR> <LAN_ENABLE_IP2> <LAN_IP_ADDR2>
	cat > "$T/cs.xml" <<XML
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="LAN_IP_ADDR" Value="$1"/>
		<Value Name="LAN_SUBNET" Value="255.255.255.0"/>
		<Value Name="LAN_ENABLE_IP2" Value="$2"/>
		<Value Name="LAN_IP_ADDR2" Value="$3"/>
		<Value Name="LAN_SUBNET2" Value="255.255.255.0"/>
	</Dir>
</Config>
XML
}
run() {
	env IFSTATE="$T/if" IFCONFIG="$T/ifconfig" SYSNET="$T/sys" CONF="$T/cs.xml" \
	    HSCONF="$T/hs.xml" OVERRIDE="$T/lan-ip" sh "$N" "$@" 2>&1
}
calls() { cat "$T/if/calls" 2>/dev/null | wc -l | tr -d ' '; }

store 192.168.0.1 0 192.168.100.1
out=$(run addr)
t "an unchanged primary is left alone" "0" "$(calls)"
t "and says so" "network: br0 keeps 192.168.0.1 netmask 255.255.255.0" "$(echo "$out" | sed -n 1p)"
t "no second address while LAN_ENABLE_IP2 is 0" "network: no second address" "$(echo "$out" | sed -n 2p)"

store 192.168.0.1 1 192.168.100.1
out=$(run addr -n)
t "the dry run says what it would add" \
  "network: would set br0:2 192.168.100.1 netmask 255.255.255.0 (was none)" "$(echo "$out" | sed -n 2p)"
t "and adds nothing" "absent" "$([ -f "$T/if/br0:2" ] && echo present || echo absent)"
run addr > /dev/null
t "LAN_ENABLE_IP2=1 adds the second address on br0:2" "192.168.100.1 255.255.255.0" "$(cat "$T/if/br0:2")"
t "without touching the primary" "192.168.0.1 255.255.255.0" "$(cat "$T/if/br0")"
t "running it again changes nothing" "1" "$(run addr > /dev/null; calls)"

store 192.168.0.1 0 192.168.100.1
run addr > /dev/null
t "LAN_ENABLE_IP2=0 removes it again" "absent" "$([ -f "$T/if/br0:2" ] && echo present || echo absent)"

store 192.168.0.1 1 192.168.0.1
t "a second address equal to the primary is not added twice" "absent" \
  "$(run addr > /dev/null; [ -f "$T/if/br0:2" ] && echo present || echo absent)"

store 192.168.0.9 1 192.168.100.1
out=$(run addr)
t "a new LAN_IP_ADDR moves the primary" "192.168.0.9 255.255.255.0" "$(cat "$T/if/br0")"
t "and the second address is put back beside it" "192.168.100.1 255.255.255.0" "$(cat "$T/if/br0:2")"

echo "10.0.0.5" > "$T/lan-ip"
store 192.168.0.1 0 192.168.100.1
out=$(run addr)
t "/etc/config/lan-ip still wins over LAN_IP_ADDR" "10.0.0.5 255.255.255.0" "$(cat "$T/if/br0")"
t "and says where the address came from" "1" "$(echo "$out" | grep -c 'address from')"
rm -f "$T/lan-ip"

rm -rf "$T/sys/br0"
echo "192.168.0.1 255.255.255.0" > "$T/if/eth0"
store 192.168.0.1 1 192.168.100.1
run addr > /dev/null
t "with no bridge the addresses go on eth0" "192.168.100.1 255.255.255.0" "$(cat "$T/if/eth0:2")"

t "an unknown mode is refused" "1" "$(run bogus > /dev/null; echo $?)"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
