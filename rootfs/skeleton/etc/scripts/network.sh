#!/bin/sh
#
# Bring up management networking: br0 over eth0, carrying the LAN address.
# Without it dropbear and telnetd listen on a stick with no routable
# interface, and a good boot looks the same as a panic from outside.
#
# br0, not the address straight on eth0, because that is the configuration the
# stock image runs on this hardware. If any bridge step fails we fall back to
# eth0: being reachable beats matching the stock layout.
#
# The address is READ, not baked in. /var/config is the jffs2 partition that
# fwu.sh never writes, so each stick keeps its value across a reflash (isp2
# answers on 192.168.1.1, isp1 on 192.168.0.1). /etc/config/lan-ip overrides.
#
# No `set -e`: this runs from rcS as sysinit, which completes BEFORE init
# starts any shell, so a failed step must not abort the boot.
#
#     network.sh            the boot bring-up: SerDes, MAC, br0, the addresses
#     network.sh addr       re-apply only the addresses, live: the primary
#                           (LAN_IP_ADDR/LAN_SUBNET, or /etc/config/lan-ip) and
#                           the second one (LAN_ENABLE_IP2, LAN_IP_ADDR2,
#                           LAN_SUBNET2). No link, MAC or SerDes is touched.
#     network.sh addr -n    say what `addr` would change, change nothing
#
# `addr` is what /etc/scripts/apply runs after the web UI saves one of those
# keys, so they take effect without a reboot. Moving the primary moves every
# way in; the second address is the one to reach the stick on meanwhile.
set -u

MODE=${1:-boot}
DRY=0
case "$MODE" in
boot) ;;
addr) [ "${2:-}" = -n ] && DRY=1 ;;
*)    echo "usage: $0 [addr [-n]]" >&2; exit 1 ;;
esac

CONF=${CONF:-/var/config/lastgood.xml}
HSCONF=${HSCONF:-/var/config/lastgood_hs.xml}
OVERRIDE=${OVERRIDE:-/etc/config/lan-ip}
IF=${IF:-eth0}
BR=${BR:-br0}
IFCONFIG=${IFCONFIG:-/sbin/ifconfig}
SYSNET=${SYSNET:-/sys/class/net}
# The second address is an alias of the device carrying the first, br0:2.
IP2_ALIAS=${IP2_ALIAS:-2}
# Per-port devices the multi-LAN driver creates; eth0.2 is the host port.
HOSTIFS=${HOSTIFS:-eth0.2}

# Last resort only. Reaching this on isp1 gives the wrong address, but a
# stick whose config partition will not read is already a bigger problem, and
# a wrong address is a symptom you can see with arping from the other end.
# No address at all is silence, which is the one outcome worth engineering out.
DEF_IP=192.168.1.1
DEF_MASK=255.255.255.0

lower() {
	echo "$1" | tr "[:upper:]" "[:lower:]"
}

xmlval() {
	[ -f "$CONF" ] || return 1
	sed -n "s/.*Name=\"$1\" Value=\"\([^\"]*\)\".*/\1/p" "$CONF" 2>/dev/null | head -n 1
}

IP=""
MASK=""

if [ -s "$OVERRIDE" ]; then
	IP=$(head -n 1 "$OVERRIDE" 2>/dev/null)
	echo "network: address from $OVERRIDE"
fi

if [ -z "$IP" ]; then
	IP=$(xmlval LAN_IP_ADDR || true)
	MASK=$(xmlval LAN_SUBNET || true)
fi

if [ -z "$IP" ]; then
	IP=$DEF_IP
	echo "network: no address in $CONF, falling back to $IP" >&2
fi
[ -n "$MASK" ] || MASK=$DEF_MASK

# The address a device holds now, as "ADDR MASK", or nothing.
addr_of() {
	$IFCONFIG "$1" 2>/dev/null |
		sed -n "s/.*inet addr:\([0-9.]*\).*Mask:\([0-9.]*\).*/\1 \2/p" | head -n 1
}

# The second management address, on the alias of the device that carries the
# first. Off unless LAN_ENABLE_IP2 is 1, which is how both our sticks ship
# (0, with 192.168.100.1/24 stored); a fixed rescue address for when the
# primary is changed to one you cannot reach. Removing it is taking the alias
# down, which drops that address and nothing else.
secondary() {   # secondary <dev>
	en=$(xmlval LAN_ENABLE_IP2 || true)
	ip2=$(xmlval LAN_IP_ADDR2 || true)
	mask2=$(xmlval LAN_SUBNET2 || true)
	[ -n "$mask2" ] || mask2=$DEF_MASK
	alias=$1:$IP2_ALIAS
	cur2=$(addr_of "$alias")
	if [ "$en" = 1 ] && [ -n "$ip2" ] && [ "$ip2" != "$IP" ]; then
		if [ "$cur2" = "$ip2 $mask2" ]; then
			echo "network: $alias keeps $ip2 netmask $mask2"
		elif [ "$DRY" = 1 ]; then
			echo "network: would set $alias $ip2 netmask $mask2 (was ${cur2:-none})"
		else
			echo "network: $alias $ip2 netmask $mask2 (second address)"
			$IFCONFIG "$alias" "$ip2" netmask "$mask2" up 2>/dev/null ||
				echo "network: could not add the second address $ip2" >&2
		fi
	else
		[ "$en" = 1 ] && [ "$ip2" = "$IP" ] &&
			echo "network: LAN_IP_ADDR2 is the primary address, not adding it twice" >&2
		if [ -n "$cur2" ]; then
			if [ "$DRY" = 1 ]; then
				echo "network: would remove $alias ${cur2% *}"
			else
				echo "network: $alias ${cur2% *} removed (LAN_ENABLE_IP2 is not 1)"
				$IFCONFIG "$alias" down 2>/dev/null ||
					echo "network: could not remove the second address" >&2
			fi
		else
			echo "network: no second address"
		fi
	fi
}

if [ "$MODE" = addr ]; then
	# Whichever device carries the primary: br0 when the boot bridged,
	# eth0 when it fell back to addressing it directly.
	dev=$IF
	[ -d "$SYSNET/$BR" ] && dev=$BR
	cur=$(addr_of "$dev")
	if [ "$cur" = "$IP $MASK" ]; then
		echo "network: $dev keeps $IP netmask $MASK"
	elif [ "$DRY" = 1 ]; then
		echo "network: would move $dev to $IP netmask $MASK (was ${cur:-none})"
	else
		echo "network: $dev $IP netmask $MASK (was ${cur:-none})"
		$IFCONFIG "$dev" "$IP" netmask "$MASK" up 2>/dev/null || {
			echo "network: could not set $IP on $dev" >&2
			exit 1
		}
	fi
	secondary "$dev"
	exit 0
fi

# The host-side SerDes, before anything else touches the link.
#
# MAC0 on this chip is the electrical side of the SFP, the only way into the
# device. The stock image sets its mode from a kernel module with no /proc
# interface here, so this script applies the stock "Fiber 1G" mode (option 1,
# the mode every working stick reports) register for register. Every target
# value was read back from a stick running that mode, with the stock diag
# and ours (all twelve matched).
#
#   HOST_SERDES_MODE 0x214   PCIE_SELECT bit15, HOLD_CLK_GLITCH bit13,
#                            PHY_SIDE_SEL bit8 all clear; LINK_MODE [4:0] = 4
#   PHY_PATCH_STATUS 0x110   PATCH_DONE bit0 = 1   ("switch ready")
#   HOST_FIBER_POWER 0x21c00 POWER_DOWN bit11 = 0
#   HOST_FIBER_TX    0x22e4c TX_MODE [15:14] = 0
#   SerDes analog    0x21680 speed select = 0         0x2168c PLL divider N = 0x30
#                    0x21698 CMU pre-divider = 0      0x215b8 TX common-mode resistor on
#                    0x215b4 TX amplitude = 2         0x215bc CMU charge pump = 0
#                    0x215e4 LDO voltage = 0
#
# (Register and field names are ours, src/diag/tools/regnames.txt. The
# analog words are not in the register table, so they go by address.)
#
# PON_SERDES_MODE 0x1d0 is not part of this mode set; the 8 it holds is not
# the mode.
#
# Every register is read first. If all of them already hold the Fiber 1G
# state (the normal case after the stock image configured it), nothing is
# written. Otherwise the stock sequence is applied whole and IN ORDER:
# the order is the mode-set procedure (park the mode at 0x1f, program the
# analog block, then select the mode), not a list of independent fixes.
# Skip everything with /etc/config/sds.off. No per-register override: a wrong
# value here takes the host link away and survives every reflash.
#
# diag is INTERACTIVE and is fed on stdin, never as bare argv. Ours exits on
# EOF; timeout is belt and braces against a hang that would stall sysinit.
sds_read() {
	printf "register get %s 1\n" "$1" | timeout 10 /bin/diag 2>/dev/null |
		sed -n "s/^0x[0-9a-fA-F]* *0x\([0-9a-fA-F]*\).*/\1/p" | head -n 1
}
sds_write() {
	printf "register set %s %s\n" "$1" "$2" | timeout 10 /bin/diag >/dev/null 2>&1 ||
		echo "network: SerDes write $1 failed" >&2
}
# read-modify-write: clear the bits in $2, set the bits in $3
sds_rmw() {
	cur=$(sds_read "$1")
	[ -n "$cur" ] || { echo "network: SerDes read $1 failed mid-sequence" >&2; return 1; }
	sds_write "$1" "$(( (0x$cur & ~$2) | $3 ))"
}
# addr mask want, one per line: the register holds want under mask.
SDS_TARGET='
0x214   0xa11f      0x4
0x110   0x1         0x1
0x21c00 0x800       0x0
0x22e4c 0xc000      0x0
0x21680 0xffffffff  0x0
0x2168c 0xffffffff  0x3001
0x21698 0xffffffff  0x68
0x215b8 0xffffffff  0x98c5
0x215b4 0xffffffff  0x1264
0x215bc 0xffffffff  0x400f
0x215e4 0xffffffff  0xfc00
'
sds_check() {
	# prints "ok", "differs: <list>" or "unreadable"
	diff=""
	for line in $(echo "$SDS_TARGET" | tr -s ' ' ',' | grep .); do
		a=${line%%,*}; rest=${line#*,}; m=${rest%%,*}; w=${rest#*,}
		cur=$(sds_read "$a")
		[ -n "$cur" ] || { echo unreadable; return; }
		[ $(( 0x$cur & m )) -eq $(( w )) ] || diff="$diff $a=0x$cur"
	done
	if [ -z "$diff" ]; then echo ok; else echo "differs:$diff"; fi
}
if [ ! -f /etc/config/sds.off ] && [ -x /bin/diag ]; then
	state=$(sds_check)
	case $state in
	ok)
		echo "network: SerDes already Fiber 1G (option 1)"
		;;
	unreadable)
		echo "network: could not read the SerDes registers, leaving them alone" >&2
		;;
	*)
		echo "network: SerDes not in Fiber 1G (${state#differs: }), applying lan_sds_mode_set(1)" >&2
		sds_rmw 0x214 0x0 0x2000 &&
		sds_rmw 0x214 0x801f 0x1f &&
		sds_rmw 0x21c00 0x800 0x0 &&
		sds_rmw 0x22e4c 0xc000 0x0 &&
		sds_write 0x21680 0x0 &&
		sds_write 0x2168c 0x3001 &&
		sds_write 0x21698 0x68 &&
		sds_write 0x215b8 0x98c5 &&
		sds_write 0x215b4 0x1264 &&
		sds_write 0x215bc 0x400f &&
		sds_write 0x215e4 0xfc00 &&
		sds_rmw 0x214 0xa11f 0x4 &&
		sds_rmw 0x110 0x0 0x1
		after=$(sds_check)
		if [ "$after" = ok ]; then echo "network: SerDes now Fiber 1G"
		else echo "network: SerDes still not Fiber 1G after the sequence ($after)" >&2; fi
		;;
	esac
fi

# The MAC, before the interface comes up.
#
# Whatever the EEPROM holds, every interface the NIC driver creates starts
# as 00:00:00:01:00:02; on the stock image only userland assigns eth0 another.
#
# Left alone, the stick is unreachable until the far end ages out its ARP
# entry for the OLD MAC, and two sticks could never share a segment.
#
# ELAN_MAC_ADDR is an HS key, so it is in lastgood_hs.xml and not the CS file
# the address comes from, and it is stored without separators.
MAC=""
if [ -f "$HSCONF" ]; then
	RAW=$(sed -n "s/.*Name=\"ELAN_MAC_ADDR\" Value=\"\([0-9a-fA-F]*\)\".*/\1/p" \
	      "$HSCONF" 2>/dev/null | head -n 1)
	if [ ${#RAW} -eq 12 ]; then
		MAC=$(echo "$RAW" | sed "s/../&:/g; s/:$//")
	fi
fi

if [ -n "$MAC" ]; then
	# Only when it differs, and only with the link DOWN: Linux refuses
	# SIOCSIFHWADDR on an interface that is up, so a second run would
	# report a failure for a MAC that is already correct.
	cur=$(/sbin/ifconfig "$IF" 2>/dev/null |
	      sed -n "s/.*HWaddr \([0-9A-Fa-f:]*\).*/\1/p" | head -n 1)
	if [ "$(lower "$cur")" = "$(lower "$MAC")" ]; then
		echo "network: $IF already has $MAC"
	else
		echo "network: $IF hw ether $MAC"
		/sbin/ifconfig "$IF" down 2>/dev/null
		/sbin/ifconfig "$IF" hw ether "$MAC" 2>/dev/null || \
			echo "network: could not set the MAC, continuing on the driver default" >&2
	fi
else
	echo "network: no ELAN_MAC_ADDR in $HSCONF -- the driver default 00:00:00:01:00:02 stands" >&2
fi

echo "network: $IF -> $BR $IP netmask $MASK"

/sbin/ifconfig "$IF" up 2>/dev/null || echo "network: $IF would not come up" >&2

bridged=0
if /sbin/brctl addbr "$BR" 2>/dev/null || /sbin/brctl show "$BR" >/dev/null 2>&1; then
	# STP off and forward delay 0: with the defaults a one-port bridge
	# still spends about fifteen seconds learning before it forwards, and
	# that is fifteen seconds of a boot looking like a failure.
	/sbin/brctl stp "$BR" off 2>/dev/null || true
	/sbin/brctl setfd "$BR" 0 2>/dev/null || true
	# Already a port of this bridge counts as success; brctl addif errors
	# on it, and a rerun would otherwise fall back to eth0 needlessly.
	#
	# Our NIC driver demuxes ingress by switch port into eth0.2 (port 0,
	# the host-side MAC) and eth0.3 (port 1); host frames arrive on eth0.2.
	# With eth0 also in br0, the switch returns our own frames on it and
	# the bridge floods out of it. So the per-port devices are the bridge
	# ports and eth0, their parent, stays up and outside; eth0 is the port
	# only when no per-port device exists (a kernel without MULTI_LAN_DEV).
	PORTS=""
	for hif in $HOSTIFS; do
		[ -d "/sys/class/net/$hif" ] && PORTS="$PORTS $hif"
	done
	[ -n "$PORTS" ] || PORTS=$IF
	for hif in $PORTS; do
		# Same MAC as eth0: the per-port devices keep the driver default
		# 00:00:00:01:00:02 otherwise, and a bridge takes the lowest MAC of
		# its ports. Set with the link down, then up.
		if [ -n "$MAC" ]; then
			/sbin/ifconfig "$hif" down 2>/dev/null
			/sbin/ifconfig "$hif" hw ether "$MAC" 2>/dev/null || \
				echo "network: could not set the MAC on $hif" >&2
		fi
		/sbin/ifconfig "$hif" up 2>/dev/null || echo "network: $hif would not come up" >&2
		if [ -d "/sys/class/net/$BR/brif/$hif" ] || /sbin/brctl addif "$BR" "$hif" 2>/dev/null; then
			echo "network: $hif -> $BR"
			bridged=1
		else
			echo "network: could not add $hif to $BR" >&2
		fi
	done
	if [ "$bridged" = 1 ]; then
		/sbin/ifconfig "$BR" "$IP" netmask "$MASK" up 2>/dev/null || bridged=0
	fi
fi

if [ "$bridged" = 0 ]; then
	echo "network: bridge path failed, addressing $IF directly" >&2
	/sbin/ifconfig "$IF" "$IP" netmask "$MASK" up 2>/dev/null || {
		echo "network: COULD NOT ASSIGN AN ADDRESS -- no way in over the network" >&2
		exit 1
	}
	secondary "$IF"
else
	secondary "$BR"
fi

exit 0
