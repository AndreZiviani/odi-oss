#!/usr/bin/env bash
# Runs INSIDE the diag toolchain container. See rcs_trace_test.sh.
#
# For each flag set: stage a root, run /etc/init.d/rcS in it under strace,
# and write the canonical action trace to /out/<set>.txt.
set -u
BB=/bb/busybox
QEMU=$(command -v qemu-mips-static)
SETS=${SETS:-none breadcrumbs confirm-arp}

# The container has no binfmt entry for MIPS, so a MIPS ELF cannot be
# execve()d directly. Every applet is therefore a two-line script whose
# interpreter is the static x86 qemu with busybox as its argument: the
# kernel runs "qemu busybox /bin/sed args", and busybox takes the applet
# name from the basename of /bin/sed. The execve that rcS makes is still
# the one to /bin/sed, which is what the trace records.
wrapper() { printf '#!/qemu-mips-static /bin/busybox\n' > "$1"; chmod 755 "$1"; }

stub() {
	# stub <path> <body>: a /bin/sh script, run through the wrapped sh.
	printf '#!/bin/sh\n%s\n' "$2" > "$1"
	chmod 755 "$1"
}

stage() {
	R=$1 set=$2
	rm -rf "$R"
	mkdir -p "$R"/{bin,sbin,lib,etc,dev,proc,sys,var,mnt,root}
	cp "$QEMU" "$R/qemu-mips-static"
	cp "$BB" "$R/bin/busybox"
	chmod 755 "$R/bin/busybox"
	# The same bin/sbin split as image/build.sh.
	for a in $("$QEMU" "$BB" --list); do
		[ "$a" = busybox ] && continue
		case "$a" in
		init|telnetd|mdev|ifconfig|route|reboot|halt|poweroff|klogd|syslogd|\
		insmod|rmmod|lsmod|modprobe|switch_root|sysctl|logread|watchdog|\
		start-stop-daemon|udhcpc|arp|nameif|vconfig|brctl|devmem|setconsole)
			wrapper "$R/sbin/$a" ;;
		*)
			wrapper "$R/bin/$a" ;;
		esac
	done
	cp -a /imagesrc/skeleton/. "$R/"
	chmod 755 "$R"/etc/init.d/* "$R"/etc/scripts/*
	rm -rf "$R/tmp"
	ln -s /var/tmp "$R/tmp"
	mkdir -p "$R"/var/{tmp,log,run,lock,config}
	echo "odi-oss-rcs-trace" > "$R/etc/version"
	printf 'CONFIG_ODI_SWITCH=y\nCONFIG_ODI_NIC=y\n' > "$R/etc/kernel-config"

	# The kernel side, as files. Reads get fixed values; writes land in a
	# regular file and are taken from the trace, not from the file.
	P=$R/proc
	mkdir -p "$P/net" "$P/sys/kernel" "$P/odi_wdt"
	# The config partition already mounted: real hardware would only reach
	# rcS's confirm_watchdog with this line present if mount -a's jffs2
	# entry actually landed, and mount is stubbed below to a blind
	# success that never updates this file itself.
	printf 'rootfs / rootfs rw 0 0\ndevtmpfs /dev devtmpfs rw 0 0\nproc /proc proc rw 0 0\nmtd:config /var/config jffs2 rw 0 0\n' > "$P/mounts"
	printf '12.34 10.00\n' > "$P/uptime"
	printf 'dev:    size   erasesize  name\nmtd0: 00040000 00010000 "boot"\nmtd3: 00100000 00010000 "config"\n' > "$P/mtd"
	printf '           CPU0\n  8:          0   rlx-irq  apl_sw\n 26:          0   rlx-irq  eth0\n' > "$P/interrupts"
	printf 'MemTotal:  28000 kB\nMemFree:   12000 kB\n' > "$P/meminfo"
	printf '0.00 0.00 0.00 1/20 100\n' > "$P/loadavg"
	printf 'Inter-|   Receive\n  eth0: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n' > "$P/net/dev"
	printf 'IP address  HW type  Flags  HW address  Mask  Device\n' > "$P/net/arp"
	for f in hotplug softlockup_thresh hung_task_timeout_secs; do : > "$P/sys/kernel/$f"; done
	echo 0 > "$P/odi_init"
	echo "registered:" > "$P/odi_omci"
	: > "$P/odi_wdt/userland_ok"

	# Device nodes, as the chroot harness makes them; kmsg a plain file.
	mknod "$R/dev/null" c 1 3; mknod "$R/dev/zero" c 1 5
	mknod "$R/dev/random" c 1 8; mknod "$R/dev/urandom" c 1 9
	mknod "$R/dev/console" c 5 1; mknod "$R/dev/tty" c 5 0
	: > "$R/dev/kmsg"
	mkdir -p "$R/dev/pts"

	# The config store gponsn/gponpw auto read, with dummy identity values.
	printf '<Value Name="GPON_SN" Value="ODIT00000001"/>\n' > "$R/var/config/lastgood_hs.xml"
	printf '<Value Name="GPON_PLOAM_PASSWD" Value="30303030303030303030"/>\n' > "$R/var/config/lastgood.xml"
	case $set in
	breadcrumbs) : > "$R/var/config/breadcrumbs.on" ;;
	confirm-arp) : > "$R/var/config/confirm-arp" ;;
	esac

	# Stubs for what needs hardware, the network or a daemon.
	stub "$R/bin/mount" 'exit 0'
	stub "$R/sbin/ifconfig" 'case "$1" in br0) echo "br0 Link encap:Ethernet"; echo "          inet addr:192.168.0.1  Bcast:192.168.0.255  Mask:255.255.255.0";; esac; exit 0'
	stub "$R/bin/arping" 'exit 0'
	stub "$R/sbin/devmem" 'case $# in 1) echo 0x00000000;; esac; exit 0'
	stub "$R/bin/diag" 'cat > /dev/null; exit 0'
	stub "$R/bin/omcid" 'exit 0'
	stub "$R/sbin/ip" 'exit 0'
	stub "$R/sbin/brctl" 'exit 0'
	# Real network.sh only leaves /var/run/network-configured when it read a
	# real address (not its DEF_IP fallback); this stub simulates that
	# configured case, since $R/var/config/lastgood.xml above has no
	# LAN_IP_ADDR but a real boot with a mounted config partition would.
	stub "$R/etc/scripts/network.sh" 'mkdir -p /var/run; : > /var/run/network-configured; exit 0'
	stub "$R/etc/init.d/services" 'exit 0'
	[ -e "$R/bin/seedrng" ] && stub "$R/bin/seedrng" 'exit 0'
	# A sleep over 5 s parks forever, so the endless background loops stop
	# at a fixed point; a short one returns at once.
	mkfifo "$R/park.fifo"
	stub "$R/bin/sleep" 'n=${1%%.*}; [ "${n:-0}" -le 5 ] && exit 0; : < /park.fifo'
}

# Waits until rcS has exited and no trace file has grown for 10 s (every
# job left is parked on the FIFO; 10 s outlasts the `timeout 5` rcS puts
# around its slow reads), with an upper bound, then stops and kills the lot.
#
# "rcS has exited" is read off the lowest-numbered trace file, not a chroot
# execve: plain `strace <cmd>` never logs the execve of the command it was
# given directly (only a traced child's own later execve calls show up), so
# grepping for `execve("/usr/sbin/chroot"...)` never matches anything and
# this used to fall through to the n<300 cap every time, taking minutes
# longer than it needed to. -ff numbers files by pid in allocation order,
# so the lowest number is always the process strace directly exec'd.
settle() {
	dir=$1
	prev="" same=0 n=0
	top=""
	while [ "$n" -lt 300 ]; do
		cur=$(cat "$dir"/t.* 2>/dev/null | wc -c)
		if [ "$cur" = "$prev" ]; then same=$((same + 1)); else same=0; fi
		[ -z "$top" ] && top=$(ls "$dir"/t.* 2>/dev/null | sort -t. -k2 -n | head -1)
		[ "$same" -ge 10 ] && [ -n "$top" ] && grep -qs "exited with" "$top" && break
		prev=$cur
		sleep 1
		n=$((n + 1))
	done
	signal_all STOP
	sleep 1
	signal_all KILL
}

# Every emulated process, found by its executable: a match on the command
# line would also hit strace, whose argv names the same qemu.
signal_all() {
	for d in /proc/[0-9]*; do
		case $(readlink "$d/exe" 2>/dev/null) in
		*/qemu-mips-static) kill -s "$1" "${d#/proc/}" 2>/dev/null ;;
		esac
	done
}

for set in $SETS; do
	R=/root/fs-$set
	T=/tmp/tr-$set
	stage "$R" "$set"
	rm -rf "$T"; mkdir -p "$T"
	strace -f -ff -y -q -s 4096 -e trace=execve,write,clone,clone3,fork,vfork \
		-o "$T/t" chroot "$R" /qemu-mips-static /bin/busybox sh /etc/init.d/rcS \
		< /dev/null > "/out/$set.console" 2>&1 &
	spid=$!
	settle "$T"
	wait "$spid" 2>/dev/null
	python3 /t/rcs_trace_canon.py "$T" "$R" > "/out/$set.txt"
	echo "$set: $(grep -c . "/out/$set.txt") actions"
done
