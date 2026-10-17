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
	    HSCONF="$T/hs.xml" OVERRIDE="$T/lan-ip" SAFE_FLAG="$T/safe-mode" sh "$N" "$@" 2>&1
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

# A value the kernel refuses is not applied, and never moves a running stick:
# the address stays, `addr` says so and fails. (The boot falls back to the
# stock default for the same values, as when the key is absent.)
mkdir -p "$T/sys/br0"
echo "192.168.0.1 255.255.255.0" > "$T/if/br0"
rm -f "$T/if/calls" "$T/if/br0:2"
for bad in 0.0.0.0 127.0.0.1 224.0.0.1 255.255.255.255 192.168.010.1 192.168.1 1.2.3.4.5 abc; do
	store "$bad" 0 192.168.100.1
	out=$(run addr); rc=$?
	t "an unusable address is refused: $bad" "1 0" "$rc $(calls)"
done
t "and the primary is where it was" "192.168.0.1 255.255.255.0" "$(cat "$T/if/br0")"
for bad in 255.0.255.0 0.0.0.0 255.255.255.255 255.255.255.254 254.0.0.0 255.255.255.1 255.255.255.00 255.255.0.128; do
	store 192.168.0.1 0 192.168.100.1
	sed "s|Name=\"LAN_SUBNET\" Value=\"[^\"]*\"|Name=\"LAN_SUBNET\" Value=\"$bad\"|" "$T/cs.xml" > "$T/cs2.xml"; cp "$T/cs2.xml" "$T/cs.xml"
	out=$(run addr); rc=$?
	t "an unusable netmask is refused: $bad" "1 0" "$rc $(calls)"
done
for good in 255.255.255.252 255.0.0.0 255.255.128.0 255.255.255.128; do
	store 192.168.0.1 0 192.168.100.1
	sed "s|Name=\"LAN_SUBNET\" Value=\"[^\"]*\"|Name=\"LAN_SUBNET\" Value=\"$good\"|" "$T/cs.xml" > "$T/cs2.xml"; cp "$T/cs2.xml" "$T/cs.xml"
	run addr > /dev/null
	t "a usable netmask is applied: $good" "192.168.0.1 $good" "$(cat "$T/if/br0")"
done
echo "192.168.0.1 255.255.255.0" > "$T/if/br0"
store 192.168.0.1 1 not-an-address
out=$(run addr)
t "an unusable second address is not added, and says so" "0 1" "$([ -f "$T/if/br0:2" ] && echo 1 || echo 0) $(echo "$out" | grep -c 'not usable')"

# A safe-mode boot carries the rescue address whatever LAN_ENABLE_IP2 says:
# the stored second address, else the stock default 192.168.100.1.
: > "$T/safe-mode"
store 192.168.0.1 0 10.9.9.1
run addr > /dev/null
t "safe mode: the rescue address is the stored second address" "10.9.9.1 255.255.255.0" "$(cat "$T/if/br0:2")"
rm -f "$T/if/br0:2"
store 192.168.0.1 0 ""
run addr > /dev/null
t "safe mode: and the stock default when none is stored" "192.168.100.1 255.255.255.0" "$(cat "$T/if/br0:2")"
rm -f "$T/safe-mode" "$T/if/br0:2"
store 192.168.0.1 0 192.168.100.1
run addr > /dev/null
t "outside safe mode LAN_ENABLE_IP2=0 adds nothing" "absent" "$([ -f "$T/if/br0:2" ] && echo present || echo absent)"

echo "10.0.0.5" > "$T/lan-ip"
store 192.168.0.1 0 192.168.100.1
t "network.sh configured fails on an unusable /etc/config/lan-ip" "1" "$(echo 0.0.0.0 > "$T/lan-ip"; run configured > /dev/null; echo $?)"
rm -f "$T/lan-ip"

t "an unknown mode is refused" "1" "$(run bogus > /dev/null; echo $?)"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
