#!/usr/bin/env bash
#
# Runs inside the privileged container. See test/fwu_stock_flash_test.sh.
set -u
TARNAME=${1:?tarball name}
pass=0
fail=0
ok()  { printf 'ok    %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }

export DEBIAN_FRONTEND=noninteractive
apt-get -qq update >/dev/null 2>&1
apt-get -qq install -y qemu-user-static python3 >/dev/null 2>&1

# Big-endian MIPS through binfmt_misc, so the stock busybox can exec its own
# applets and the stub helpers.
# binfmt_misc is KERNEL-GLOBAL, not per container. On Docker Desktop every
# container shares one VM kernel, so an entry registered here stays registered
# for every other container until something removes it -- and with the `F`
# flag the interpreter is held open, so it works even where qemu is not
# installed. Leaving it behind made rootfs_chroot_test.sh start EXECUTING the
# MIPS `diag` it had always failed to exec, which turned ten of its assertions
# red with an arithmetic error that had nothing to do with the change under
# test. Register it, and take it away again however this script exits.
unregister_qmips() {
	[ -e /proc/sys/fs/binfmt_misc/qmips ] && echo -1 > /proc/sys/fs/binfmt_misc/qmips
	losetup -D 2>/dev/null
	return 0
}
trap unregister_qmips EXIT

mount -t binfmt_misc none /proc/sys/fs/binfmt_misc 2>/dev/null
printf '%s' ':qmips:M::\x7fELF\x01\x02\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x08:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-mips-static:F' \
	> /proc/sys/fs/binfmt_misc/register 2>/dev/null
[ -e /proc/sys/fs/binfmt_misc/qmips ] || { echo "FAIL  could not register binfmt for mips"; exit 1; }

R=/root/stick
rm -rf "$R"; mkdir -p "$R"
cp -a /stick/. "$R"/
mkdir -p "$R/proc" "$R/dev" "$R/tmp" "$R/t" "$R/usr/bin"
cp /usr/bin/qemu-mips-static "$R/usr/bin/"
mknod -m 666 "$R/dev/urandom" c 1 9 2>/dev/null
mknod -m 666 "$R/dev/random"  c 1 8 2>/dev/null
mknod -m 666 "$R/dev/null"    c 1 3 2>/dev/null
mknod -m 666 "$R/dev/zero"    c 1 5 2>/dev/null
mount -t proc proc "$R/proc" 2>/dev/null

if chroot "$R" /bin/ash -c 'exit 0' 2>/dev/null; then
	v=$(chroot "$R" /bin/ash -c 'busybox 2>&1 | sed -n "1p"')
	ok "the stock shell runs: $v"
else
	bad "the stock busybox will not run at all -- the harness is broken"
	printf '\n%d passed, %d failed\n' "$pass" "$fail"
	exit 1
fi

cp "/tars/$TARNAME" "$R/t/"
cat > "$R/t/mtd" <<'EOF'
dev:    size   erasesize  name
mtd4: 0014c000 00001000 "k0"
mtd5: 00274000 00001000 "r0"
mtd6: 0014c000 00001000 "k1"
mtd7: 00274000 00001000 "r1"
EOF
echo "console=ttyS0,115200 root=31:5 rw" > "$R/t/cmdline"

# Loop-backed partitions, pre-erased to 0xff. A regular file would be
# truncated by the write and the read-back would never see a real tail.
python3 -c "
open('/root/p6.img','wb').write(b'\xff'*0x14c000)
open('/root/p7.img','wb').write(b'\xff'*0x274000)"
# The container may have no /dev/loopN nodes, and a previous run's may still be
# attached on the host. Make our own and release them on the way out.
for i in 0 1 2 3 4 5 6 7; do
	[ -e "/dev/loop$i" ] || mknod "/dev/loop$i" b 7 "$i" 2>/dev/null
done
losetup -D 2>/dev/null
L6=$(losetup --find --show /root/p6.img) || { echo "FAIL  losetup (no free loop device)"; exit 1; }
L7=$(losetup --find --show /root/p7.img) || { echo "FAIL  losetup (no free loop device)"; exit 1; }
rm -f "$R/dev/mtd6" "$R/dev/mtd7"
mknod "$R/dev/mtd6" b "$(stat -c '%t' "$L6")" "$(stat -c '%T' "$L6")"
mknod "$R/dev/mtd7" b "$(stat -c '%t' "$L7")" "$(stat -c '%T' "$L7")"
chmod 666 "$R/dev/mtd6" "$R/dev/mtd7"
ok "two loop-backed partitions, pre-erased to 0xff"

cat > "$R/bin/flash_eraseall" <<'STUB'
#!/bin/sh
echo "flash_eraseall: erasing $1 (stub)"
exit 0
STUB
cat > "$R/bin/nv" <<'STUB'
#!/bin/sh
[ "$1" = getenv ] || exit 1
case "$2" in
sw_commit) echo 0 ;;
sw_active) echo 0 ;;
*) exit 1 ;;
esac
STUB
chmod +x "$R/bin/flash_eraseall" "$R/bin/nv"

# 1. The tarball has to unpack with the vendor's own tar.
if chroot "$R" /bin/ash -c "cd /t && tar -xf $TARNAME" 2>&1 | grep -q .; then
	bad "the stock tar complained while unpacking the image"
else
	ok "the stock busybox tar unpacks the image silently"
fi
for m in fwu.sh fwu_ver md5.txt rootfs uImage; do
	[ -f "$R/t/$m" ] || bad "member $m did not extract"
done
[ -f "$R/t/uImage" ] && [ -f "$R/t/rootfs" ] && ok "all five members extracted"

# 2. The whole script, erase and write included.
out=$(chroot "$R" /bin/ash -c \
	"cd /t && PROC_MTD=/t/mtd PROC_CMDLINE=/t/cmdline ./fwu.sh 1 $TARNAME" 2>&1)
rc=$?
printf '%s\n' "$out" | sed 's/^/      /'
if [ "$rc" -eq 0 ]; then
	ok "fwu.sh ran to completion under the stock shell"
else
	bad "fwu.sh exited $rc"
fi
printf '%s\n' "$out" | grep -q 'not found' &&
	bad "something was 'not found' -- a command the stock image does not have"
printf '%s\n' "$out" | grep -q 'uImage verified on /dev/mtd6' ||
	bad "the uImage read-back did not report verified"
printf '%s\n' "$out" | grep -q 'rootfs verified on /dev/mtd7' ||
	bad "the rootfs read-back did not report verified"

# 3. The bytes. Not "it said it worked" -- what is actually on the partitions.
if python3 - <<'PY' 
import sys
rc = 0
for member, img, size in (("uImage", "/root/p6.img", 0x14c000),
                          ("rootfs", "/root/p7.img", 0x274000)):
    try:
        m = open("/root/stick/t/" + member, "rb").read()
        d = open(img, "rb").read()
    except OSError as e:
        print(f"FAIL  {member}: cannot check the bytes -- {e.strerror}: {e.filename}")
        rc = 1
        continue
    if len(d) != size:
        print(f"FAIL  {member}: partition is {len(d)} bytes, want {size}"
              " -- the write TRUNCATED it, so this run proved less than it looks")
        rc = 1
        continue
    if d[:len(m)] != m:
        print(f"FAIL  {member}: the bytes on the partition are not the member")
        rc = 1
        continue
    tail = set(d[len(m):])
    if not tail <= {0xff}:
        print(f"FAIL  {member}: the tail past the member is not erased flash")
        rc = 1
        continue
    print(f"ok    {member}: {len(m)} bytes written into {size}, tail still 0xff")
sys.exit(rc)
PY
then
	pass=$((pass + 2))
else
	fail=$((fail + 1))
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
