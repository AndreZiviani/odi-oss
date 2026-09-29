# memprobe and the RAM log console

`memprobe` is a small static MIPS tool (built from `memprobe.c` with the command below; the binary is not committed) that reads and writes physical memory
through `/dev/mem` on the stick, from ANY kernel that runs there, the stock
(OEM) one included. It exists because the device has no serial console:
`CONFIG_ODI_RAMLOG` (`kernel/extra/drivers/net/ethernet/odi/odi_ramlog.c`)
mirrors every console line into two DRAM pages that nothing else uses and
that survive a reset -- physical `0x017ff000` (the first 3984 bytes of the
log, then a 32-byte reset reason block and a 64-byte boot metadata block;
4016 bytes and no reason block from format 1 images, 4080 bytes of log
from older ones) and `0x01fff000` (a ring of the last 4080) -- so after a trial boot
reverts, the old image can read what the new kernel said. When the old
image is ours as well, `/proc/odi_ramlog_prev` already holds the same two
pages as the reverted boot left them (`docs/KERNEL.md`), no memprobe needed.

Build (our toolchain, static):

    docker run --rm -v "$PWD/tools/memprobe:/w" \
      "$(packages/oss-env.sh)" sh -c 'cd /w && mips-linux-uclibc-gcc -std=gnu99 -Os -static -o memprobe memprobe.c && mips-linux-uclibc-strip memprobe'

Use:

    ssh <stick> 'cat > /tmp/memprobe && chmod +x /tmp/memprobe' < tools/memprobe/memprobe
    memprobe r  017ff000 1000          # hexdump head + checksum of a page
    memprobe w  017ff000 1000 TAG      # fill a page with a tag (pre-tag before a trial)
    memprobe d  017ff000 1000 > page   # raw dump
    memprobe reg 18001004              # one 32-bit register by physical address
    memprobe set 1B010004 A0000000     # write one register, read it back
    CTL=/tmp/odi_ctl tools/memprobe/ramlog-read.sh out/dir   # both pages, decoded

`ramlog-read.sh` runs `/tmp/memprobe d` on the stick over an existing ssh
control socket (`CTL`, default `/tmp/odi_ctl`: open one first with
`ssh -M -S /tmp/odi_ctl -fN root@<stick>`), saves `pageA.bin` and
`pageB.bin` in the directory given, and prints: both page headers, the
early crumb in page B (`CONFIG_ODI_EARLY_CRUMBS`, tag and step, while it is
still ASCII), the page A metadata block when its `RLGM` magic is there
(boot counter, slot, build id, and the crumb stash at page A `+4088`: the
last crumb of the boot before), then the page A text and the page B ring
in order. `kernel/extra/drivers/net/ethernet/odi/odi_ramlog.h` has the
layout.

From one of our own images there is no need to push anything:
`/proc/odi_ramlog_prev` is the same decoding, done by the kernel, of the
pages as the previous boot left them, and `/proc/odi_ramlog_prev_raw` is the
8192 raw bytes (page A, then page B), so
`head -c 4096` and `tail -c 4096` of it are the two `.bin` files above.

Read `docs/FLASHING.md` for how this fits the trial procedure. It has caught
kernel bugs across several trial boots that were not visible any other way.

Do not push more than half of the free memory into the ramfs of a running
stock (OEM) image: pushing 1.5 MB into 1.1 MB free has rebooted a stick.
Drop caches first (`echo 3 > /proc/sys/vm/drop_caches`) and read `MemFree`.
