#!/usr/bin/env bash
# Runs INSIDE the toolchain container. See rootfs_chroot_test.sh.
set -u
pass=0; fail=0
R=/root/fs

note() { printf '\n== %s\n' "$1"; }
ok()   { echo "ok    $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL  $1"; shift; [ $# -gt 0 ] && printf '%s\n' "$*" | sed 's/^/        /'; fail=$((fail + 1)); }

# A writable copy: the real root is squashfs, but the scripts mkdir and mknod,
# and a read-only copy would fail for the wrong reason.
cp -a /stage "$R"
# qemu-mips-static has to be reachable from inside the chroot.
cp "$(command -v qemu-mips-static)" "$R/qemu-mips-static"
mkdir -p "$R/proc" "$R/sys" "$R/dev"

# rcS mounts a ramfs on /var and makes these five at boot. A chroot cannot
# mount anything, so they are made here -- otherwise `services` fails on
# /var/log/modules.log and /tmp (a symlink to /var/tmp) is a dangling link,
# and both read as image defects when they are harness artefacts.
mkdir -p "$R/var/tmp" "$R/var/log" "$R/var/run" "$R/var/lock" "$R/var/config"

# THE EMULATOR NEEDS ENTROPY BEFORE THE GUEST DOES. qemu-mips-static aborts at
# startup with
#
#     qemu-mips-static: cannot initialize crypto: No /dev/urandom or
#     /dev/random: No such file or directory
#
# if NEITHER node is present in the chroot. That message is qemu talking, not
# the guest, and an earlier version of this harness mistook it for proof that
# dropbearkey fails without entropy -- it was a false pass that would have
# survived any image change at all. So the nodes are created here, up front,
# before a single guest binary runs.
#
# The two are separable, which is what makes the real test below honest:
# dropbear reads /dev/urandom ONLY, so leaving /dev/random in place keeps qemu
# happy while taking the guest entropy source away.
mknod "$R/dev/random"  c 1 8 2>/dev/null
mknod "$R/dev/urandom" c 1 9 2>/dev/null
mknod "$R/dev/null"    c 1 3 2>/dev/null
mknod "$R/dev/zero"    c 1 5 2>/dev/null
mknod "$R/dev/console" c 5 1 2>/dev/null
mknod "$R/dev/tty"     c 5 0 2>/dev/null

inchroot() { chroot "$R" /qemu-mips-static "$@"; }
sh_in()    { chroot "$R" /qemu-mips-static /bin/busybox sh "$@"; }

# `services` backgrounds metricsd, confd, dropbear and omcid. A command
# substitution would inherit its stderr into a pipe those daemons hold open
# forever, and the harness would hang rather than fail -- it did. Capture to a
# FILE, whose fd the daemons may keep as long as they like.
runlog() {
	log=$1; shift
	chroot "$R" /qemu-mips-static "$@" >"$log" 2>&1 </dev/null
}

note "the shell and busybox run at all"
v=$(inchroot /bin/busybox 2>&1 | head -1)
case "$v" in BusyBox*) ok "busybox: $v" ;; *) bad "busybox does not run" "$v" ;; esac

note "the applet list, which everything below depends on"
applets=$(inchroot /bin/busybox --list 2>/dev/null)
na=$(printf '%s\n' "$applets" | grep -c .)
if [ "$na" -gt 50 ]; then
	ok "busybox --list answers with $na applets"
else
	bad "busybox --list returned $na applets -- every applet test below would" \
	    "be a false FAIL, so stopping here rather than printing a wall of them."
	echo; echo "$pass passed, $fail failed"; exit 1
fi

note "every command the boot scripts call exists as an applet or a file"
# Absolute paths only, deliberately: our scripts spell them out because init
# PATH does not carry /etc/scripts, so anything relative is a shell builtin or
# a keyword and not our problem.
#
# /etc/init.d and /etc/scripts, NOT /etc. A bare /etc matched /etc/inittab and
# every /etc/config/*.off flag -- those are data the scripts read or create at
# runtime, not commands, and reporting them as missing binaries was noise that
# buried the one real answer.
#
# COMMENTS ARE STRIPPED FIRST. Our scripts explain themselves, and the
# explanations name paths -- flash says why it does not ship /bin/xmlconfig,
# and the scanner duly reported /bin/xmlconfig as a missing command. A checker
# that reads prose will keep inventing findings, so it reads code only.
paths=$(sed 's/#.*//' /stage/etc/init.d/* /stage/etc/scripts/* 2>/dev/null |
        grep -oE '(^|[[:space:]]|\$\()/(bin|sbin|usr/bin|usr/sbin|etc/init\.d|etc/scripts)/[A-Za-z0-9_./-]+' |
        sed -E 's/^.*(\/(bin|sbin|usr|etc))/\1/' | sort -u)
missing=
for c in $paths; do
	[ -e "/stage$c" ] || missing="$missing $c"
done
if [ -z "$missing" ]; then ok "every absolute path a boot script calls exists"
else bad "boot scripts call paths that are not in the image" "$missing"; fi

note "the flasher's own tools"
for a in flash_eraseall md5sum tar dd head wc; do
	if printf '%s\n' "$applets" | grep -qx "$a"; then ok "$a"; else bad "$a is not an applet"; fi
done

note "every boot script parses under the shell that will run it"
for f in /stage/etc/init.d/* /stage/etc/scripts/*; do
	[ -f "$f" ] || continue
	n=${f#/stage}
	if out=$(sh_in -n "$n" 2>&1); then ok "sh -n $n"; else bad "sh -n $n" "$out"; fi
done

note "/tmp is writable"
# The stock vendor image ships `tmp -> /var/tmp`, and /var is the ramfs rcS
# mounts. Ours has a real /tmp directory instead, which lands in the squashfs
# and is therefore read-only forever. Anything that writes a temp file loses.
if [ -L "$R/tmp" ]; then
	ok "/tmp is a symlink to $(readlink "$R/tmp")"
elif [ -d "$R/tmp" ]; then
	bad "/tmp is a real directory, so it is read-only squashfs at runtime" \
	    "the stock image ships tmp -> /var/tmp, and /var is the ramfs rcS mounts"
else
	bad "/tmp is neither a directory nor a symlink"
fi

note "the entropy node is in the image, and only the one"
# devices.pseudo is the ONLY source of device nodes: /dev/urandom has no sysfs
# entry, so nothing populating /dev from /sys will ever create it.
if grep -qE "^/dev/urandom[[:space:]]" /imagesrc/devices.pseudo 2>/dev/null; then
	ok "/dev/urandom is in the image"
else
	bad "/dev/urandom is NOT in the image" \
	    "nothing creates it at boot either -- see the dropbearkey test below"
fi
# And /dev/random must NOT be. It is the blocking pool; a one-byte read never
# returns on this hardware, and a read inside rcS would hang sysinit and take
# the console, telnet and ssh with it. Nothing here reads it: dropbear uses
# DROPBEAR_URANDOM_DEV with no fallback, and busybox seedrng, the only other
# reference in the rootfs, reads its seed with getrandom() and opens
# /dev/random only when that syscall is missing (ENOSYS, never on 6.18), and
# then only to poll it with a zero timeout.
if grep -qE "^/dev/random[[:space:]]" /imagesrc/devices.pseudo 2>/dev/null; then
	bad "/dev/random is in the image" \
	    "it blocks forever; anything that reads it hangs the boot"
else
	ok "/dev/random is deliberately absent"
fi
# rcS seeds the pool with seedrng at boot, so the applet has to be there.
if [ -x "$R/bin/seedrng" ]; then
	ok "seedrng is built, for the rcS entropy seed (it never reads /dev/random)"
else
	bad "busybox seedrng is missing" \
	    "rcS skips the entropy seed and every daemon starts on a nearly empty pool"
fi

note "the device nodes the rest of this file needs"
# The kernel provides /dev on the device (devtmpfs, mounted before init); a
# chroot has to make the nodes itself. qemu-mips-static needs one of the two
# entropy sources to start at all.
mknod "$R/dev/random"  c 1 8 2>/dev/null
mknod "$R/dev/urandom" c 1 9 2>/dev/null
mknod "$R/dev/null"    c 1 3 2>/dev/null
mknod "$R/dev/zero"    c 1 5 2>/dev/null
mknod "$R/dev/console" c 5 1 2>/dev/null
mknod "$R/dev/tty"     c 5 0 2>/dev/null
if [ -c "$R/dev/null" ] && [ -c "$R/dev/urandom" ]; then
	ok "/dev/null and /dev/urandom are character devices"
else bad "the chroot has no /dev/null or /dev/urandom" "$(ls -l "$R/dev" 2>&1)"; fi

note "dropbearkey without /dev/urandom"
# dropbear seeds from getrandom(), so the node is no longer what host key
# generation stands or falls on (under qemu the syscall reaches the host).
# /dev/random stays, so qemu itself still starts. /var/tmp and not /tmp: the
# OUTER shell resolves $R/tmp against the container root, not the chroot.
rm -f "$R/dev/urandom"
out=$(chroot "$R" /qemu-mips-static /sbin/dropbearkey -t ed25519 -f /var/tmp/k1 2>&1)
rc=$?
case "$out" in
	qemu-mips-static:*)
		bad "the EMULATOR failed, not dropbearkey -- this test proves nothing" "$out" ;;
	*)
		if [ "$rc" = 0 ] && [ -s "$R/var/tmp/k1" ]; then ok "dropbearkey generates a key with no /dev/urandom (getrandom)"
		else bad "dropbearkey failed with no /dev/urandom" "$out"; fi ;;
esac
rm -rf "${R:?}/etc/config"; mkdir -p "$R/etc/config"
runlog /tmp/svc-nokey.log /bin/busybox sh /etc/init.d/services start
if [ -s "$R/etc/config/dropbear.d/ed25519" ]; then
	ok "services start writes a host key with no /dev/urandom"
else bad "services start wrote no host key with no /dev/urandom" "$(cat /tmp/svc-nokey.log)"; fi

note "dropbearkey with /dev/urandom"
mknod "$R/dev/urandom" c 1 9 2>/dev/null
if chroot "$R" /qemu-mips-static /sbin/dropbearkey -t ed25519 -f /var/tmp/k2 >/dev/null 2>&1 &&
   [ -s "$R/var/tmp/k2" ]; then ok "generates a key ($(stat -c %s "$R/var/tmp/k2") bytes)"
else bad "dropbearkey failed even with /dev/urandom"; fi

note "rcS, run for real"
# The mounts are the host's and cannot be redone inside a chroot, so they are
# stubbed. Everything else runs as written, rcS.dev included.
sed -e 's|^/bin/mount |true # |' "$R/etc/init.d/rcS" > "$R/etc/init.d/rcS.test"
runlog /tmp/rcs.log /bin/busybox sh -x /etc/init.d/rcS.test
rc=$?
if [ "$rc" = 0 ]; then ok "rcS exits 0"
else bad "rcS exits $rc" "$(tail -15 /tmp/rcs.log)"; fi
grep -E "not found|No such file|Syntax error|unexpected" /tmp/rcs.log | sort -u | sed 's/^/        rcS: /'

note "how many pty pairs rcS actually made"
# rcS creates 16 when devtmpfs has none. Counting them here proves only
# that the loop runs; which ones open is a question for a real boot.
p=$(find "$R/dev" -name 'ptyp*' 2>/dev/null | wc -l)
if [ "$p" -gt 0 ]; then ok "$p ptyp nodes created (nodes, not working ptys)"
else bad "rcS created no pty nodes at all"; fi

note "services start, run for real"
rm -rf "$R/etc/config"; mkdir -p "$R/etc/config"
runlog /tmp/svc.log /bin/busybox sh -x /etc/init.d/services start
rc=$?
if [ "$rc" = 0 ]; then ok "services start exits 0"
else bad "services start exits $rc" "$(tail -15 /tmp/svc.log)"; fi
grep -E "not found|No such file|Syntax error|unexpected" /tmp/svc.log | sort -u | sed 's/^/        services: /'
if [ -s "$R/etc/config/dropbear.d/ed25519" ]; then
	ok "a host key was generated ($(stat -c %s "$R/etc/config/dropbear.d/ed25519") bytes)"
else
	bad "services start generated no host key even with /dev/urandom present"
fi

# The OTHER half, because a trial boot depends on it. docs/FLASHING.md says to
# `touch /etc/config/dropbear.off` on the running stick before flashing, so
# that the new image does not write a host key to mtd3 -- the one partition
# fwu.sh never touches and the self-reverting trial therefore does not undo.
# Checked as a PAIR with the run above: a test that only shows nothing was
# written cannot tell the switch being off from services being broken.
rm -rf "$R/etc/config/dropbear.d"
: > "$R/etc/config/dropbear.off"
runlog /tmp/svc2.log /bin/busybox sh /etc/init.d/services start
if [ -e "$R/etc/config/dropbear.d/ed25519" ]; then
	bad "dropbear.off did not stop the host key being written" \
	    "that write lands on mtd3, which the trial revert does not cover"
else
	ok "dropbear.off stops the host key being written at all"
fi
rm -f "$R/etc/config/dropbear.off"

note "the boot leaves something readable behind"
# A trial boot has no serial console here, so anything printed and not stored
# is lost. These two are what a first boot can be diagnosed from over ssh.
if grep -q "services.log" "$R/etc/init.d/rcS"; then
	ok "rcS captures services output to /var/log/services.log"
else bad "services output goes only to a console nobody can read"; fi
if grep -q "hostname -F" "$R/etc/init.d/rcS"; then
	ok "rcS applies /etc/hostname"
else bad "hostname file ships but is never applied"; fi

note "our own binaries answer"
for b in diag nv omcli igmpd; do
	if [ -x "$R/bin/$b" ]; then
		chroot "$R" /qemu-mips-static "/bin/$b" --help >/dev/null 2>&1
		ok "$b runs (exit $?)"
	else bad "$b is not in the image"; fi
done

note "network.sh picks the right address"
# What is tested here is the half that can be silently wrong: WHICH address it
# chooses, and that it says so. The interface work itself needs real hardware.
#
# IF is pointed at a name that cannot exist, for two reasons. Docker gives the
# container a REAL eth0, so the default would have the test reconfigure the
# container out from under itself -- and an earlier version of this block did
# exactly that, then passed, because ifconfig succeeded. And with no usable
# interface the fallback path runs to its end, which is the one branch worth
# proving: the script must fail loudly rather than silently, and must not hang.
export IF=odi-no-such-if
mkdir -p "$R/var/config" "$R/etc/config" 2>/dev/null
cat > "$R/var/config/lastgood.xml" <<'XML'
<Config>
  <Value Name="LAN_IP_ADDR" Value="192.168.0.1"/>
  <Value Name="LAN_SUBNET" Value="255.255.255.0"/>
</Config>
XML
runlog /tmp/net1.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "192.168.0.1 netmask 255.255.255.0" /tmp/net1.log; then
	ok "reads LAN_IP_ADDR and LAN_SUBNET out of the config store"
else bad "did not read the address from lastgood.xml" "$(cat /tmp/net1.log)"; fi

# The SerDes. A STATEFUL stub diag stands in, because the real one needs the
# switch driver -- what is under test is the read, the decision and the exact
# ORDER of the writes, which is where a mistake would take away the only
# interface that reaches the box. The stub keeps a register file so the
# read-modify-write steps of the sequence see their own earlier writes, as
# they would on hardware.
mkdir -p "$R/var/tmp"
cat > "$R/stubdiag" <<'STUB'
#!/bin/sh
read -r line
set -- $line
norm() { printf "0x%x" "$(( $1 ))"; }
case "$1 $2" in
"register get")
	a=$(norm "$3")
	v=$(grep "^$a=" /var/tmp/regs 2>/dev/null | head -n 1 | cut -d= -f2)
	[ -n "$v" ] && printf "0x%08x 0x%08x\n" "$(( a ))" "$(( v ))"
	;;
"register set")
	a=$(norm "$3"); v=$(printf "0x%x" "$(( $4 ))")
	echo "set $a $v" >> /var/tmp/sdsset
	grep -v "^$a=" /var/tmp/regs > /var/tmp/regs.n 2>/dev/null; echo "$a=$v" >> /var/tmp/regs.n
	mv /var/tmp/regs.n /var/tmp/regs
	;;
esac
STUB
chmod +x "$R/stubdiag"
cp "$R/bin/diag" "$R/bin/diag.real" 2>/dev/null
cp "$R/stubdiag" "$R/bin/diag"
# The Fiber 1G state as read off isp1 2026-09-15, with both diags.
fiber1g() {
	cat > "$R/var/tmp/regs" <<'REGS'
0x214=0x260004
0x110=0x1
0x21c00=0x1140
0x22e4c=0x31b
0x21680=0x0
0x2168c=0x3001
0x21698=0x68
0x215b8=0x98c5
0x215b4=0x1264
0x215bc=0x400f
0x215e4=0xfc00
0x1d0=0x8
REGS
}

note "SerDes: the working state is recognised and left alone"
fiber1g; rm -f "$R/var/tmp/sdsset"
runlog /tmp/sds1.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "SerDes already Fiber 1G" /tmp/sds1.log && [ ! -f "$R/var/tmp/sdsset" ]; then
	ok "twelve registers at the Fiber 1G values: no write"
else bad "wrote to the SerDes when it was already Fiber 1G" "$(cat /tmp/sds1.log; cat "$R/var/tmp/sdsset" 2>/dev/null)"; fi

note "SerDes: bits outside the compared fields do not trigger a rewrite"
fiber1g; rm -f "$R/var/tmp/sdsset"
sed -i 's/^0x214=.*/0x214=0xdead0004/; s/^0x21c00=.*/0x21c00=0x7ff/' "$R/var/tmp/regs"
runlog /tmp/sds2.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "SerDes already Fiber 1G" /tmp/sds2.log && [ ! -f "$R/var/tmp/sdsset" ]; then
	ok "compares the fields lan_sds sets, not the whole word"
else bad "rewrote on bits outside the fields" "$(cat /tmp/sds2.log)"; fi

note "SerDes: a stick in SGMII MAC gets the vendor sequence, in the vendor order"
fiber1g; rm -f "$R/var/tmp/sdsset"
# mode 3 as lan_sds_mode_set(3) leaves it: [4:0]=2, and a different analog block
sed -i 's/^0x214=.*/0x214=0x260002/; s/^0x215b4=.*/0x215b4=0x1564/; s/^0x21c00=.*/0x21c00=0x1940/' "$R/var/tmp/regs"
runlog /tmp/sds3.log /bin/busybox sh /etc/scripts/network.sh
# Expected, computed from the starting values by the same arithmetic
# network.sh performs: 0x260002 -> set bit13 -> 0x262002 -> clear
# bit15, [4:0]=0x1f -> 0x26201f; 0x1940 clear bit11 -> 0x1140; 0x31b clear
# [15:14] -> 0x31b; seven raw writes; 0x26201f clear 15,13,8 set [4:0]=4 ->
# 0x260004; 0x1 set bit0 -> 0x1.
cat > /tmp/sds-want <<'WANT'
set 0x214 0x262002
set 0x214 0x26201f
set 0x21c00 0x1140
set 0x22e4c 0x31b
set 0x21680 0x0
set 0x2168c 0x3001
set 0x21698 0x68
set 0x215b8 0x98c5
set 0x215b4 0x1264
set 0x215bc 0x400f
set 0x215e4 0xfc00
set 0x214 0x260004
set 0x110 0x1
WANT
if grep -q "applying lan_sds_mode_set(1)" /tmp/sds3.log && cmp -s /tmp/sds-want "$R/var/tmp/sdsset"; then
	ok "the thirteen writes of lan_sds_mode_set(1), in order, with the right values"
else bad "the write sequence is not the vendor one" "$(cat /tmp/sds3.log; echo ---; diff /tmp/sds-want "$R/var/tmp/sdsset" 2>&1)"; fi
if grep -q "SerDes now Fiber 1G" /tmp/sds3.log; then ok "and reads back as Fiber 1G afterwards"
else bad "did not read back as Fiber 1G after the sequence" "$(cat /tmp/sds3.log)"; fi

note "SerDes: unreadable registers mean hands off"
rm -f "$R/var/tmp/regs" "$R/var/tmp/sdsset"
runlog /tmp/sds4.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "could not read the SerDes registers" /tmp/sds4.log && [ ! -f "$R/var/tmp/sdsset" ]; then
	ok "no switch answering: nothing written"
else bad "wrote to the SerDes with nothing to read" "$(cat /tmp/sds4.log)"; fi

note "SerDes: sds.off suppresses everything"
fiber1g; sed -i 's/^0x214=.*/0x214=0x260002/' "$R/var/tmp/regs"; rm -f "$R/var/tmp/sdsset"
touch "$R/etc/config/sds.off"
runlog /tmp/sds5.log /bin/busybox sh /etc/scripts/network.sh
if [ ! -f "$R/var/tmp/sdsset" ] && ! grep -q "SerDes" /tmp/sds5.log; then
	ok "sds.off: no read, no write"
else bad "sds.off did not stop the SerDes code" "$(cat /tmp/sds5.log)"; fi
rm -f "$R/etc/config/sds.off" "$R/var/tmp/regs"
mv "$R/bin/diag.real" "$R/bin/diag" 2>/dev/null
rm -f "$R/stubdiag"

# The MAC comes from the HS file, not the CS one, and is stored unseparated.
cat > "$R/var/config/lastgood_hs.xml" <<'XML'
<Config>
  <Value Name="ELAN_MAC_ADDR" Value="aabbccddeeff"/>
</Config>
XML
runlog /tmp/netmac.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "hw ether aa:bb:cc:dd:ee:ff" /tmp/netmac.log; then
	ok "reads ELAN_MAC_ADDR out of lastgood_hs.xml and separates it"
else bad "did not build the MAC from the HS config" "$(cat /tmp/netmac.log)"; fi
rm -f "$R/var/config/lastgood_hs.xml"
runlog /tmp/netnomac.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "no ELAN_MAC_ADDR" /tmp/netnomac.log; then
	ok "says so when there is no MAC to read, rather than silently defaulting"
else bad "silent when ELAN_MAC_ADDR is missing" "$(cat /tmp/netnomac.log)"; fi

echo "10.0.0.7" > "$R/etc/config/lan-ip"
runlog /tmp/net2.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "10.0.0.7" /tmp/net2.log; then
	ok "/etc/config/lan-ip overrides the config store"
else bad "the lan-ip override was ignored" "$(cat /tmp/net2.log)"; fi
rm -f "$R/etc/config/lan-ip"

rm -f "$R/var/config/lastgood.xml"
runlog /tmp/net3.log /bin/busybox sh /etc/scripts/network.sh
if grep -q "falling back to 192.168.1.1" /tmp/net3.log; then
	ok "falls back to a default, loudly, when there is no config"
else bad "no fallback when the config store is missing" "$(cat /tmp/net3.log)"; fi

# The one that matters most: rcS runs as sysinit under `set -e`, so a
# network.sh that aborts the boot costs every way back into the device.
if grep -q "COULD NOT ASSIGN AN ADDRESS" /tmp/net3.log; then
	ok "says so unmistakably when it cannot address anything"
else bad "silent failure when no interface can be addressed" "$(cat /tmp/net3.log)"; fi

if grep -q "network.sh" "$R/etc/init.d/rcS"; then
	ok "rcS calls network.sh"
else bad "rcS does not call network.sh -- the image would boot unreachable"; fi
if grep -q "network.sh || " "$R/etc/init.d/rcS"; then
	ok "rcS guards the call, so a failure cannot abort sysinit"
else bad "rcS calls network.sh unguarded under set -e"; fi
unset IF

note "the config accessor runs under busybox ash on MIPS"
# test/flash_test.sh covers the behaviour on the host. What that cannot show is
# that the script is interpreted correctly by the shell which will actually run
# it -- busybox ash, big-endian MIPS, through qemu. Two cases are enough for
# that: one read and one write, since between them they exercise sed, the
# pattern quoting, the temp file and the rename.
cat > "$R/var/config/lastgood.xml" <<'XML'
<Config>
  <Value Name="LAN_IP_ADDR" Value="192.168.0.1"/>
  <Value Name="VLAN_MANU_TAG_VID" Value="11"/>
</Config>
XML
runlog /tmp/flash1.log /bin/busybox sh /etc/scripts/flash get LAN_IP_ADDR
if grep -q "LAN_IP_ADDR=192.168.0.1" /tmp/flash1.log; then
	ok "flash get, under the target shell"
else bad "flash get did not work under busybox ash" "$(cat /tmp/flash1.log)"; fi

runlog /tmp/flash2.log /bin/busybox sh /etc/scripts/flash set VLAN_MANU_TAG_VID 10
if grep -q "VLAN_MANU_TAG_VID=10" /tmp/flash2.log &&
   grep -q 'Name="VLAN_MANU_TAG_VID" Value="10"' "$R/var/config/lastgood.xml" &&
   grep -q 'Name="LAN_IP_ADDR" Value="192.168.0.1"' "$R/var/config/lastgood.xml"; then
	ok "flash set rewrites one key and leaves the rest alone"
else bad "flash set misbehaved under busybox ash" "$(cat /tmp/flash2.log)"; fi
rm -f "$R/var/config/lastgood.xml"

note "ptys: the half that had no coverage at all"
# telnetd is the documented fallback way into the device and nothing ever
# tested it. It cannot be exercised in a chroot -- it needs a real pty driver
# and a network listener -- so what is checked here is the set of conditions
# that made it impossible: busybox takes the /dev/ptmx path, and until
# 2026-09-14 neither the node nor the driver behind it existed.
if grep -aq "/dev/ptmx" "$R/bin/busybox"; then
	ok "busybox is built for the /dev/ptmx pty path"
	if [ -e "$R/dev/ptmx" ] || grep -q "^/dev/ptmx[[:space:]]" /imagesrc/devices.pseudo; then
		ok "/dev/ptmx is in the image"
	else bad "busybox wants /dev/ptmx and the image does not ship it -- telnetd dies per connection"; fi
elif grep -aq "/dev/pty%c%c" "$R/bin/busybox"; then
	ok "busybox uses the BSD pty scan, /dev/ptmx not required"
else
	bad "cannot tell which pty path busybox takes -- neither string is in the binary"
fi
if grep -q "mount -t devpts" "$R/etc/init.d/rcS"; then
	ok "rcS mounts devpts for the pty slave side"
else bad "nothing mounts devpts; /dev/pts/N will not appear"; fi

# dropbear opens /dev/ptmx on the devpts rcS mounts, and can fall back to
# the BSD pairs rcS makes.
if grep -aq "/dev/ptmx" "$R/sbin/dropbearmulti"; then
	ok "dropbear takes its ptys from /dev/ptmx (devpts)"
else bad "dropbear does not open /dev/ptmx -- check its pty path against rcS"; fi

# The count has to cover what rcS creates, or the extra nodes are ENODEV.
n=$(sed -n 's/^CONFIG_LEGACY_PTY_COUNT=//p' "$R/etc/kernel-config" 2>/dev/null)
if [ "${n:-0}" -ge 16 ]; then
	ok "CONFIG_LEGACY_PTY_COUNT is $n, covering the 16 pairs rcS makes"
else bad "LEGACY_PTY_COUNT (${n:-unset}) is below the 16 pairs rcS makes"; fi

# The daemons services started are still alive and would keep the container up.
pkill -f qemu-mips-static >/dev/null 2>&1

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
