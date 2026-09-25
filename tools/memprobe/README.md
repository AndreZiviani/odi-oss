# memprobe and the RAM log console

`memprobe` is a 36 KB static MIPS tool that reads and writes physical memory
through `/dev/mem` on the stick, from ANY kernel that runs there, the stock
(OEM) one included. It exists because the device has no serial console:
`CONFIG_ODI_RAMLOG` (`kernel/extra/drivers/net/ethernet/odi/odi_ramlog.c`)
mirrors every console line into two DRAM pages that nothing else uses and
that survive a reset -- physical `0x017ff000` (the first 4016 bytes of the
log, then a 64-byte boot metadata block; 4080 bytes of log from older
images) and `0x01fff000` (a ring of the last 4080) -- so after a trial boot
reverts, the old image can read what the new kernel said. When the old
image is ours as well, `/proc/odi_ramlog_prev` already holds the same two
pages as the reverted boot left them (`docs/KERNEL.md`), no memprobe needed.

Build (our toolchain, static):

    docker run --rm -v "$PWD/tools/memprobe:/w" -v odi-oss-toolchain-318:/tc:ro \
      "$(packages/oss-env.sh)" sh -c 'cd /w && /tc/bin/mips-linux-uclibc-gcc -std=gnu99 -Os -static -o memprobe memprobe.c && /tc/bin/mips-linux-uclibc-strip memprobe'

Use:

    ssh <stick> 'cat > /tmp/memprobe && chmod +x /tmp/memprobe' < tools/memprobe/memprobe
    memprobe r  017ff000 1000          # hexdump head + checksum of a page
    memprobe w  017ff000 1000 TAG      # fill a page with a tag (pre-tag before a trial)
    memprobe d  017ff000 1000 > page   # raw dump
    memprobe reg 18001004              # one 32-bit register by physical address
    memprobe set 1B010004 A0000000     # write one register, read it back
    CTL=/tmp/odi_ctl tools/memprobe/ramlog-read.sh out/dir   # both pages, decoded

Read `docs/FLASHING.md` for how this fits the trial procedure. It has caught
kernel bugs across several trial boots that were not visible any other way.

Do not push more than half of the free memory into the ramfs of a running
stock (OEM) image: pushing 1.5 MB into 1.1 MB free has rebooted a stick.
Drop caches first (`echo 3 > /proc/sys/vm/drop_caches`) and read `MemFree`.
