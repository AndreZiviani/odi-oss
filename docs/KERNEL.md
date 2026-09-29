# The kernel

Linux **6.18.53**, the current longterm release, built for the RTL9602C.
There is no dual-kernel build any more: `kernel/build.sh` builds 6.18 and
nothing else, from a pristine `cdn.kernel.org` tree with our own patches and
drivers on top — no vendor kernel tree is fetched, patched or linked at any
point in this build.

    kernel/618/fetch.sh    fetches and verifies linux-6.18.53.tar.xz
    kernel/618/mainline/   the pristine tree it extracts, gitignored
    kernel/618/patches/    our edits to mainline files, applied in order
    kernel/618/debug/      debug-only edits, applied with CRUMBS_CORE=1
    kernel/extra/          every file of our own, laid over the tree at its path
    kernel/618/config      the seed config; make olddefconfig completes it
    kernel/build.sh        builds it all, in a container, with our toolchain

## Patches and the overlay

The port is split by what it costs to carry to the next longterm kernel.
A file of our own is copied across; a line changed in a file mainline
already has must be re-applied, and re-read, against new code. So:

- `kernel/extra/` holds every file this port adds, at its path in the
  kernel tree: `arch/mips/rtl8686/` (the board file, the RLX5281 cache
  code `cache.c`, the interrupt controller `irq.c`, the TIMER0 clock
  event `time.c`, Kconfig, Makefile and Platform),
  `arch/mips/include/asm/mach-rtl8686/` (kernel entry, feature overrides,
  IRQ numbering, register map, the early-crumb helpers),
  `drivers/mtd/devices/rtl8686-spiflash.c` and our driver directory
  `drivers/net/ethernet/odi/`. `kernel/build.sh` copies every file but the
  `README.md` files to the same path under the build tree, after the
  patches, on every run; an edit to an overlay file alone rebuilds
  incrementally. Adding or removing an overlay file changes the patch
  stamp and re-extracts the tree, so a deleted file does not linger.
- `kernel/618/patches/` holds only edits to files mainline already has:
  the hooks that make the overlay reachable (Kconfig entries and
  `source` lines, Makefile `obj-` lines) and the few core changes the CPU
  needs. `0001` is the RLX5281 PRId probe case, `0002` the board's
  System type entry, its Kconfig `source` and the `Kbuild.platforms` line, `0004` the SPI NOR driver's Makefile line and
  `0005` the two lines that reach our driver directory. The numbers are
  stable names; gaps are patches that no longer exist.
- `kernel/618/debug/` holds patches the normal build does not apply
  (see Early crumbs below).

`tools/kernel-footprint.sh` counts the second kind, per patch: the lines
changed in mainline files and the number of those files. Run it after
touching a patch; the number is the porting cost.

Every file in `kernel/618/patches/` and `kernel/618/debug/` is `git
format-patch` style: `From:`, `Date:`, `Subject:` headers, the commit body,
a bare `---` separator line, then the plain unified diff -- no `diff --git`
extended header (`kernel/build.sh` applies them with `patch -p1`, not
`git am`, so it does not need one) and deliberately no `Signed-off-by:`
line: this project signs commits with the maintainer's own SSH key, not
the kernel community's DCO process, so a sign-off line here would assert
a process this repo does not follow. `checkpatch.pl --no-tree` on these
files is otherwise clean (aside from `TRAILING_WHITESPACE` inherited from
mainline context lines, which is not a patch-format issue and is left
alone).

## The board

A single fixed board, `MACH_RTL8686`: 32 MB DRAM, one UART, SPI NOR flash, no
device tree. There is no generic "RTL8686 platform" to configure around —
the board file registers exactly this stick's UART and flash controller and
reserves the PON DMA windows, and the boot command line (which slot, which
MAC, etc.) comes from U-Boot's `argv`, built per boot slot.

Two things in the board file exist because the hardware is not idle when
this kernel starts:

- **The NIC DMA is stopped in `prom_init()`.** The loader that runs before
  us leaves the CPU-port NIC's receive engine running, on its own
  descriptor ring and buffers. `odi_nic` resets and reprograms the NIC
  only at its first `ndo_open`, from rcS, so until then every frame that
  arrived was written through the loader's descriptors: the payload into
  its buffers, the descriptor write-back into its ring slot, both in pages
  this kernel had long since handed out. The symptom was a page of
  `/bin/busybox` text in the page cache overwritten after init started, and
  every exec of it taking the same SIGBUS until the watchdog reset the
  board, in roughly one boot in six on some kernel layouts. `prom_init()`
  now writes 0 to the NIC's two RUN registers (`RTL8686_NIC_RUN`,
  `RTL8686_NIC_RUN1`, `rtl8686regs.h`) with two uncached stores, before
  this kernel owns a single page; 20 of 20 boots of the same layout were
  clean after it.
- **The PBO DMA windows are reserved where the hardware has them.** The
  PON MAC's packet-buffer engine owns two DRAM ranges, each 1 MiB plus a
  4 KB barrier page: downstream `[0x016ff000, 0x01800000)` and upstream
  `[0x01eff000, 0x02000000)`, the bases every replayed switch init writes
  to the two PBO base registers. `plat_mem_setup()` `memblock_reserve()`s
  both. The downstream reservation used to start one page higher, at
  `0x01700000`, which gave the engine's first page to the page allocator.
  The two ramlog pages (below) are the barrier pages of these windows, so
  the same reservations protect them.

## The CPU

The RLX5281 is a Lexra core, R3000-class rather than a full MIPS32
implementation: it keeps CP0 `Status` as a KU/IE stack, returns from
exceptions with `rfe`, and uses a 6-bit ASID. The kernel is built as
plain `CONFIG_CPU_R3000` (the board selects `SYS_HAS_CPU_R3000`), so
mainline's own R3000 branches supply that exception model, the TLB code
and the context switch; no core file is edited for it. `objdump -d
vmlinux | grep -cw eret` must print 0. Its caches are driven
through a Lexra CP0 register (CCTL) and are board code:
`arch/mips/rtl8686/cache.c`, installed from `plat_mem_setup()`, before
mainline's own `cpu_cache_init()` (which then finds nothing to install
for this CPU and only builds the page protection map: the feature
overrides set `cpu_has_3k_cache` to 0, so the R3000 cache code, built
under `CPU_R3000`, never installs).

The icache does not snoop the dcache, and nothing in mainline
invalidates it when a physical page gets new contents. Every path that
does give a page new contents for a process ends in
`flush_data_cache_page()` before the page is mapped: `__update_cache()`
from `set_ptes()` for a page cache folio that `flush_dcache_folio()`
marked (squashfs, jffs2 and tmpfs fills, `write()`, a folio compaction
migrated -- `CONFIG_COMPACTION` is on), and `copy_user_highpage()` for
every COW copy. R3K TLBs have no no-exec bit (`_PAGE_NO_EXEC` is 0, so
`__update_cache()` treats every mapping as executable), and without
RIXI every binary lacking `PT_GNU_STACK` runs READ_IMPLIES_EXEC, so
nearly every VMA is `VM_EXEC`. Our `flush_data_cache_page()` writes the
page's dcache lines back (per-line op 0x1b through the KSEG0 alias,
which also drops the icache lines at that colour), drains the write
buffer with a `sync`, then invalidates the whole icache through CCTL.
`flush_cache_page()` on a `VM_EXEC` mapping (COW, reclaim, migration,
and `copy_to_user_page()` after a ptrace or `/proc/pid/mem` write) does
the same. `prom_free_prom_memory()`, the board hook `free_initmem()` calls
just before the `__init` sections are freed, flushes both caches once;
it is not `__init` itself, so no init line is refilled after it.

Why whole-cache: the per-line invalidate reaches only lines at the
colour of the address it is given. How the icache is indexed is not
documented in anything we have and was never measured; 64 KB with 32-byte
lines would need 16 ways to be free of aliases at 4 KB pages, so a
virtually indexed one very likely has 8 KB or more per way, and lines of
a physical page filled through a user mapping at another colour survive
an invalidate through the kernel alias. The fix assumes nothing about the
indexing: a whole-cache invalidate is correct for any of them. Not a
concern: an ASID-tagged virtual icache (no flush on ASID wrap,
`cpu_has_vtag_icache` 0) would fail on most boots, since rcS wraps the 64
ASIDs many times.

The history, corrected. This work started from boots where init or
busybox `login` died of SIGBUS right after `Run /sbin/init`, and a stale
icache fitted the first of them. The later ones showed that memory itself
was wrong: `print-fatal-signals=1` (see "The config") printed the
registers, and the faulting word in the page cache decoded as an RX
descriptor. The real cause was the NIC DMA the loader leaves running (see
"The board"), which is now stopped. The icache change stays: the hole it
closes is real (nothing else invalidates the icache when a page gets new
contents) and it cost no measurable boot time, but it is not what made
those boots fail.

Cost. CCTL clears the icache as one operation; no document we have gives
its cycle count, and it is at most one cycle per line (2048 lines) if it
walks the array. The real cost is the refill afterwards: the kernel and
the faulting process fetch their working set again, a few hundred 32-byte
lines, on the order of 10-50 us per invalidate at this clock. It runs
once per newly filled page cache page at its first mapping, once or twice
per COW fault, and once per page reclaim or compaction unmaps from an
executable mapping; back-to-back invalidates cost only the lines fetched
between them. Boot (rcS forks a few hundred commands) should grow by well
under a second; steady state, with a handful of long-lived processes,
does almost none of these. The per-line hooks for kernel addresses
(`flush_icache_range()`, the DMA hooks) are unchanged.

Barriers are the one place where the core is not an R3000.
`CPU_R3000` turns `CPU_HAS_SYNC` off and `CPU_HAS_WB` on. Without
`CPU_HAS_SYNC`, mainline `wmb()`, `rmb()`, `dma_wmb()` and `__sync()`
emit no instruction. With `CPU_HAS_WB`, `mb()`, `iob()` and the barrier
`readl()`/`writel()` issue before each access call the board's
`__wbflush`, which `board.c` points at a `sync`: the RLX5281 implements
`sync`, and it drains the write buffer. So every MMIO access and every
`mb()`/`iob()` (the cache code's DMA hooks included) still issues a
`sync`, through an indirect call. Our own DMA-ordering points (a NIC
descriptor body before its ownership bit, the returned RX ownership bits
before the engine stops) use `rtl8686_sync()`
(`asm/mach-rtl8686/rtl8686-barrier.h`), an inline `sync` that does not
depend on `CPU_HAS_SYNC`; a plain `wmb()` there would silently be
nothing. The only `sync` this build drops is the `rmb()` at the end of
each `readl()`/`readw()`/`readb()`, which keeps later loads of DMA
memory from being satisfied ahead of the MMIO read. The NIC does not
depend on it: its rings come from `dma_alloc_coherent()` (uncached), and
its packet buffers are invalidated through the cache hooks, which do
issue a `sync`. Whether this core lets a load pass an outstanding
uncached load has not been measured. `BUG_ON()`
must stay trap-free: at `-march=mips2` mainline builds it on `tne`, one of
the instructions this core does not implement. `kernel/build.sh` passes
`-U_MIPS_ISA -D_MIPS_ISA=_MIPS_ISA_MIPS1` to the kernel C code, so
`asm/bug.h` falls back to the generic `if (cond) BUG()` form with no patch;
code generation is unchanged. `docs/CROSS-COMPILING.md`
has the full list of what this CPU does and does not implement, and why
every binary in the image is checked against it.

The core has to be named in the probe. It reports PRId `0x0000dc02`:
company byte 0 (legacy), implementation `0xdc`. Mainline builds
`cpu-r3k-probe.c` instead of `cpu-probe.c` whenever `CPU_R3K_TLB` is set,
and that file only knows the R2000 and R3000 implementation numbers. With
no case for `0xdc` the probe leaves the CPU name NULL, and the `BUG_ON()`
right after it executes a `break` before `trap_init()` has installed any
exception vector: a silent hang with nothing on any console. The fields
the probe sets are what the core has: a TLB of 64 entries, R3000-style
caches, no FPU. It does not call `__cpu_has_fpu()`, which toggles CP0
`Status` CU1 and reads it back, a sequence never tried on this core;
`cpu_has_fpu` is 0 at compile time anyway.

## IRQ and timer

The board's own interrupt controller (`arch/mips/rtl8686/irq.c`, called
from `arch_init_irq()`) is a legacy IRQ domain fed from GIMR/GISR
registers, routing to CPU interrupt lines IP2–IP7. The periodic tick
(`arch/mips/rtl8686/time.c`, from `plat_time_init()`) comes from hardware
timer TIMER0, programmed once for a fixed
HZ-periodic interrupt — this SoC's timer is not a general comparator, so
there is no dynamic `set_next_event()`. There is also no free-running counter
usable as a clocksource; timekeeping falls back to the kernel's own jiffies
clocksource.

## NOR flash

The SPI NOR flash is not a memory-mapped CFI chip: the SoC SPI controller
issues discrete command/address/data SPI transactions through a control
and a data register, and a third register opens a memory-mapped read
window on the same controller. `rtl8686-spiflash.c` (in the overlay, built
by `patches/0004`) is a real MTD driver for it, registered as `rtk_spi_nor_mtd` — the name
U-Boot's own partition table (`mtdparts`) expects — so the fourteen
partitions U-Boot already knows about (two kernel slots, two rootfs slots,
the bootloader environment in duplicate, the JFFS2 config store, and some
small placeholders) show up unchanged under Linux.

## Our drivers

`kernel/extra/drivers/net/ethernet/odi/` is the one driver directory this
port adds, hooked into `drivers/net/ethernet/` by `patches/0005`. Every file
in it is GPL-2.0 and our own code, an independent implementation written
from public specifications (ITU-T G.984.3 and G.988 for GPON and OMCI,
SFF-8472 for optics), from register traces taken off this board's own
hardware, and from behaviour observed on the stock firmware. It is not an
optional alternative to a vendor driver: on this kernel tree there is no
vendor driver to fall back to, so this is simply how the board's networking
and GPON hardware are driven.

| file | what it drives |
|---|---|
| `odi_nic.c` | the CPU-port Ethernet DMA engine (rings, NAPI, TX/RX) |
| `odi_switch.c`, `odi_switch_{tbl,dal,cmd}.c` | the switch fabric: MMIO map, indirect table access, the OMCI command dispatch `omcid` drives it through |
| `odi_switch_l2.c` | the L2 lookup table: row readback and the valid-row walk (the MAC table), L2 multicast add/delete (igmpd) |
| `odi_gpon*.c` | the GPON MAC block: the O1–O7 activation state machine and PLOAM codec (ITU-T G.984.3), register leaves, the interrupt handler, and the switch interrupt line, whose one consumer it is |
| `odi_omci.c` | the netlink transport that carries OMCI frames between the kernel and `omcid` |
| `odi_board.c`, `odi_board_data.c` | board-init: LED and I2C core bring-up, an ordered 88-write replay built into the kernel (it runs before any filesystem) |
| `odi_init.c`, `odi_switch_sdkinit.c`, `odi_gpon_init.c` | `/proc/odi_init`, the SDK-init and PON verbs rcS writes, and the register replays behind them |
| `odi_replay.c` | the one replay loop every captured write sequence goes through (`docs/SWITCH.md`, "Register replays") |
| `odi_soc.c` | the SoC system-controller window: one `ioremap()`, named offsets, the one allowlist every driver access there goes through (the replays, the watchdog, the optics) |
| `odi_replay_blob.c`, `odi_replay_fw.c` | the replay tables as firmware files: parser and loader (below) |
| `odi_i2c.c`, `odi_ddm.c` | the I2C bus to the optical module, and SFF-8472 DDM (temperature, voltage, bias, tx/rx power) readout |
| `odi_reg.c` | `/dev/odi_sw`: register/SoC/MIB/DDM/L2-table access for our own userland (`diag`, `metricsd`, `igmpd`) |
| `odi_wdt.c` | the watchdog kicker — see below |
| `odi_ramlog.c` | the DRAM ring-buffer console — see below |

The switch/GPON/OMCI/board pieces are all built statically into `vmlinux`;
there are no loadable kernel modules for the datapath, and none of this
repo's build can produce one — `image/build.sh` stages zero `.ko` files.
Every `CONFIG_ODI_*` symbol is `bool`, so nothing is exported either: the
drivers call each other through plain declarations in shared headers
(`odi_nic.h`, `odi_switch_api.h`).

### Register replay tables are firmware files

The three captured write sequences the drivers replay — the SDK-init
verbs (`sdkinit.bin`, 267 KB), the module-load replay (`modload.bin`,
130 KB) and the GPON boot init (`gpon_init.bin`, 91 KB) — are not compiled
into the kernel. They ship in the rootfs as `/lib/firmware/odi/*.bin`
(committed under `rootfs/skeleton/lib/firmware/odi/`) and are loaded with
`request_firmware()` when a trigger needs one, then released: nothing
stays resident, which gave back about 484 KB of RAM and 8.5 KB of uImage.
Every trigger comes after the root filesystem is mounted: the sdkinit and
modload replays run from rcS writes to `/proc/odi_init` and
`/proc/odi_omci`, and the GPON init runs once, at the first activation of
a boot (it is loaded in process context before the GPON lock is taken).
Later re-activations take their own path and do not reload it.
`modload.bin` holds exactly what a boot replays: the selection that used
to be a boot-time mask is applied to the file (`replayblob.py filter`,
`docs/SWITCH.md`), and the kernel replays every record.

The format is our own (`odi_replay_blob.h` is the byte-level reference,
`tools/regtrace/replayblob.py` writes and dumps it): a 24-byte big-endian
header (magic, version, table id, header size, record size, count, a
reserved word, and a CRC-32 over the header and every record) followed by
36-byte records, parsed byte by byte. A truncated, corrupt or
mis-generated file is refused whole, never half applied.
`CONFIG_FW_LOADER` is on for this (the Kconfig `select`s it) with plain
filesystem lookup only: no user-mode helper, no compressed firmware, no
firmware cache. `test/odi_replay_blob_test.sh` (in `make test-host`) runs
the C parser and the GPON init replay against the host mock, and has the
Python writer validate and dump each committed file, so the writer and
the reader are checked against the same bytes.

### Locking

The switch has one set of shared engines — the indirect table access, the
command state, the multi-register sequences, the I2C master — reached from
netlink (`omcid`), `/dev/odi_sw` (`diag`, `metricsd`), `/proc` writes and
the GPON interrupt path. `odi_switch.c` has the full comment; in short:

- `odi_switch_lock`, a mutex, serialises the table engine and every
  multi-register sequence. Process context only; it may sleep inside
  (the replays call `request_firmware()`).
- `odi_switch_dsf_lock`, an IRQ-safe spinlock, covers the few registers
  the GPON atomic path does touch: the downstream GEM and Alloc-ID CAM
  handshakes, the flow type and slot map, the encryption bits. It is a
  leaf, and nothing is printed under it.
- `odi_i2c_lock`, a mutex, covers the whole I2C byte sequence (setup,
  address, start, poll, read, per byte): the DDM ioctl, the `/proc`
  readers, the transceiver command and the sdkinit I2C verbs. Without it
  a `diag` reading and an exporter DDM poll could interleave and read each
  other's bytes.
- `odi_wdt_flag_lock` serialises the watchdog enable writers, so two can
  no longer each start a kicker thread.

The order is `odi_switch_lock -> odi_i2c_lock`, `odi_switch_lock ->
odi_gpon_lock -> odi_switch_dsf_lock` and, from the hard IRQ,
`odi_gpon_lock -> odi_switch_dsf_lock`; the host mock
aborts on a recursive acquire or on releasing a lock not held.

### The OMCI netlink socket needs `CAP_NET_ADMIN`

`odi_omci` speaks on its own private netlink protocol number (`NETLINK_ODI`;
the stock firmware used `NETLINK_USERSOCK`), which like any netlink
protocol accepts senders without any privilege. Every message that
registers for a redirect type or sends a driver command is refused unless
the sender has `CAP_NET_ADMIN` (`netlink_capable()`), so only root can
answer the OLT or drive the switch
through it.

### The NIC

`odi_nic.c` is a small driver for one DMA engine, but three of its error
paths were wrong before this release, none seen on a stick: an RX refill
that failed to allocate handed the hardware a descriptor still pointing at
a freed buffer (it now maps the replacement first and, on failure, drops
the frame and recycles the mapped buffer); a full TX ring stopped the
queue with nothing to wake it (there is no TX-complete interrupt, so a
10 ms delayed work reclaims while a queue is stopped, and the queue wakes
at half ring); and no DMA mapping was checked (`dma_mapping_error()` now
guards all three). The init error path now unwinds in reverse.
`kernel/extra/drivers/net/ethernet/odi/README.md` has the rest.

### The L2 table

`odi_switch_l2.c` reads the switch L2 lookup table (1,024 hashed rows, four
ways per bucket, then 64 CAM rows, walked while L2_LOOKUP_SETUP.CAM_OFF is
clear -- it is, on this image) through the shared indirect table engine at 0x012000, one row per
access. A row is 78 bits in three words, read back from TABLE_READ_WORD
0..2 in that order with no reversal; `odi_switch_l2.h` has the field table.

**Validity is the engine answer, not a row bit.** After a by-row read,
TABLE_STATUS (0x012004) has HIT (bit 12) set when the row holds an entry and
clear when it is empty; its low ten bits echo the row read either way. Bit
77, the valid flag a write sends, reads back set on every row. Checked on
ISP1 (image 618p2) with four learned addresses, reading all 1,024 rows and
TABLE_STATUS after each:

    row    status      raw (bits 95..64 63..32 31..0)     decodes as
    0x06c  0x0000106c  0x00002038 0x0000bc24 0x1105b324   BC:24:11:05:B3:24 port 0 age 7
    0x270  0x00001270  0x0000203d 0x000e00e4 0x064cc6f4   00:E4:06:4C:C6:F4 port 2 VID 14 C-tag
    0x364  0x00001364  0x00002038 0x0000049f 0xca787282   04:9F:CA:78:72:82 port 0 age 7
    0x390  0x00001390  0x0000203e 0x0000383a 0x212827c8   38:3A:21:28:27:C8 port 3 age 7
    other  the row     0x00002000 0x00000000 row / 4      nothing: 1,020 rows, HIT clear

HIT was set after exactly those four reads, and on none of the 64 CAM rows
(read the same way: leftover words, bit 77 clear, TABLE_STATUS.IN_CAM set). The 1,020 empty rows all read
0x00002000 0x00000000 and their bucket number (row / 4) in word 0 -- the low
MAC octet -- which suggests the table stores only the part of the MAC its
bucket does not already imply and rebuilds the rest on the way out. A driver
that took bit 77 for validity (618p2 did) lists every row of the table as a
learned `00:00:00:00:00:NN`. The host test (`test/odi_switch_l2_test.c`)
models the table this way and replays the readout above.

**Keys.** All four learned rows are SVL (bit 63 clear) on filtering id 0,
the VID-14-tagged one from the PON included; on a unicast row bits 48..59
hold the VID of the learned frame, not a lookup key. So every VLAN on this
image is shared, and the multicast entries igmpd writes are keyed the same
way: SVL, filtering id 0, static (bit 62), member ports in bits 66..69, and
valid (bit 77) set; a delete sends the key alone (MAC, filtering id, IVL)
with valid clear. Both go through the engine hash (TABLE_CMD method 0) and
report the row from TABLE_STATUS. On ISP1, `igmpd -w -j 239.1.2.3 -p 0x1`
landed on row 0x17c as 0x00002004 0x40000100 0x5e010203, and `-l` returned
that row to empty.

HIT after a hash *write* is not "the key was there": a delete write of a
key never in the table comes back with HIT set and the row the key hashes
to. A hash *read* (method 0, read, the key in TABLE_WRITE_WORD) does
answer it -- HIT and the row for a present key, HIT clear for an absent
one -- so a delete looks the key up first and writes only when it is
found.

## Building it

    make toolchain       # once: pull the pinned gcc/binutils/uClibc-ng image
    make kernel           # kernel/build.sh

`kernel/build.sh` runs in a container, using our own toolchain
(`toolchain/README.md`), with

    -march=mips2 -mno-branch-likely -mdivide-breaks

on top of the kernel's own flags — the same suppressions every other
component in this image needs; `docs/CROSS-COMPILING.md` explains why. The
seed config (`kernel/618/config`) is completed with `olddefconfig`, and the
uImage's entry point is read back out of the build's own `System.map`
(`kernel_entry`) rather than assumed, because it does not sit at the load
address on this kernel line. Output is `build/kernel-618/uImage`, plus
`System.map` and the `.config` actually used — both are picked up
automatically by `image/build.sh`, which ships the `.config` on the device
as `/etc/kernel-config` for reference.

`VERSION=<image version> kernel/build.sh` stamps that version into the
ramlog metadata block (below) as the build id; without it the kernel
takes the same `odi-oss-<date>-<rev>` default `image/build.sh` uses.
`CRUMBS_CORE=1` also applies `kernel/618/debug/` (see "Early crumbs").

## The config

`kernel/618/config` is a fragment, not a full `.config`: `olddefconfig`
completes it. It is sized for a 1328 KB kernel partition: `-Os`, dead code
elimination, `SLUB_TINY`, no modules, no kallsyms, a 16 KB printk ring
(`LOG_BUF_SHIFT=14`; the ramlog keeps the boot log anyway), IPv4, bridge
and 802.1Q only. Every line carries its reason in a comment there.

**The syscall diet.** Options whose syscalls nothing in the rootfs can
reach are off: `AIO`, `SIGNALFD`, `TIMERFD`, `EVENTFD`, `INOTIFY_USER`,
`FHANDLE`, `ADVISE_SYSCALLS`, `RSEQ`, `MEMBARRIER`,
`CROSS_MEMORY_ATTACH`, `PROC_PAGE_MONITOR`, `COREDUMP`,
`ETHTOOL_NETLINK` and `SWAP`. "Unreachable" was established by scanning
every ELF in the rootfs for the syscall number loaded right before each
`syscall` instruction, using the o32 numbers (4000 + N), and by grepping
the scripts for the tools that would need each one. A few of those
syscalls are compiled into a shipped binary but reached only by a path this
image never runs (`inotify_init1` in `ip netns monitor`, `madvise` in
busybox's yescrypt code, the handle syscalls in iproute2's cgroup2
helpers); the config comments say which.

Deliberately kept on, and why:

- `POSIX_TIMERS`: on 6.18 it also builds `kernel/time/itimer.c`, so with
  it off `alarm` and `setitimer` return `ENOSYS`, and busybox `arping`,
  `ping` and `timeout` (rcS uses `arping`) stop timing out.
- `FILE_LOCKING`: busybox `flock` and iproute2 reach `flock`, and `fcntl`
  record locks cannot be ruled out by a syscall-number scan at all.
- `VM_EVENT_COUNTERS`: the `/proc/vmstat` event counters are the memory
  pressure signal the exporter is meant to read. About 1.4 KB.
- `SYSVIPC`: omcid serves `omcli`/`omcicli` over a SysV message queue.
- `MULTIUSER`: dropbear needs the uid/gid plumbing even for a root-only
  login.

**Debug aids that stay on.** `print-fatal-signals=1` is on the built-in
command line: a process killed by SIGSEGV, SIGBUS or SIGILL prints its
registers (`epc`, `ra`, `Status`, `Cause`, `BadVA`) to the console, and so
to the ramlog, where a dying init otherwise leaves only "Attempted to kill
init!". The hung-task detector (30 s) and the soft-lockup detector are on
for the same reason, and so are the early crumbs (`CONFIG_ODI_EARLY_CRUMBS`,
below).

**The command line.** `CONFIG_CMDLINE` is only the slot-0 fallback
(`root=31:5` and the `mtdparts=` table). With
`MIPS_CMDLINE_BUILTIN_EXTEND` it goes first and U-Boot's per-slot
arguments after it, so the bootloader's `root=` and `mtdparts=` win (the
last occurrence of each is the one used); the default ordering made the
slot-0 `root=` win on a slot-1 boot.

## The boot command line and the watchdog

U-Boot passes a per-slot command line in `argv`: which root device
(`root=31:5` for slot 0, `root=31:7` for slot 1), and the rest of what
`prom_init` needs. Nothing in the kernel hardcodes a slot.

U-Boot arms the hardware watchdog before handing over to a **trial** boot
(`nv setenv sw_tryactive <slot>`, see `docs/FLASHING.md`); the kernel's job
is to keep kicking it. `odi_wdt.c` is a small, from-scratch kicker thread:
it arms the watchdog itself at init, on every boot, at the same operating
point U-Boot's own `en_wdt` sets, and — critically — it also tracks a
**userland confirmation** deadline, not just "the kernel is alive." Unless
something writes `1` to `/proc/odi_wdt/userland_ok` within 120 s of
uptime, the kernel forces the reset itself. rcS writes it once its own
steps have run (the network is not a condition; with the development
flag `/etc/config/confirm-arp` it waits for an ARP reply from the `.2`
address of the `br0` subnet instead), so a kernel that boots fine but
whose init scripts hang still gets reset. Before that
forced reset, the watchdog calls `wdt_pre_reset_hook`, which `odi_nic.c`
sets to its own quiesce function, so the NIC's DMA engine is stopped the
same way an orderly shutdown would stop it. The proc directory keeps the
stock firmware's name so rcS works on both.

### Reboot, halt and power-off

The watchdog is the only reset this SoC has, so `odi_wdt.c` also registers
the kernel's restart handler (`register_restart_handler`, priority 128, the
level for a handler that restarts the whole system). The board file sets no
`_machine_restart` or `_machine_halt` hook, so mainline `machine_restart()`
runs that handler once the reboot notifiers and device shutdown are done, or
straight away on an emergency restart (panic reboot, sysrq). The handler
masks interrupts, calls `wdt_pre_reset_hook` (the NIC DMA stops; the NIC
driver reboot notifier has already done that on an orderly reboot, but an
emergency restart skips the notifiers), writes the watchdog control
register with ENABLE alone (`TIMEOUT1=0`, the shortest timeout) and kicks,
the same sequence the userland deadline uses. The kick matters: the
timeout fields do not restart the count. Measured on the stick, the control
write alone reset the board about 1.05 s later, and the write followed by a
kick about 0.33 s later. If the board were still running 3 s later, the
handler re-arms the watchdog at its normal operating point and returns;
`machine_restart()` then hangs with interrupts masked and the reset comes
one window later.

Before this, `reboot` ended in a board hook that spun with interrupts off
until the watchdog window ran out: about 42 s of a dead stick on every
reboot, on top of U-Boot and the next boot.

Halt and power-off have nothing to act on: the stick has no power switch,
and a halted stick in an SFP cage is useful only once it resets. A reboot
notifier re-arms the watchdog at its normal operating point on both (even
if `watchdog_flag` had turned it off), and mainline `machine_hang()` then
spins with interrupts masked, so nothing kicks it: the board resets about
42 s later, and a trial falls back to the committed slot as on any hang.
The NIC reboot notifier stops its DMA on these paths too.

## The DRAM ramlog console

This device has no serial console, so a boot that never comes up on the
network leaves nothing to look at — unless something outside the network
stack recorded it. `odi_ramlog.c` (`CONFIG_ODI_RAMLOG`) is a `struct console`
driver that mirrors every kernel console line into two fixed physical DRAM
pages (`0x017ff000`, the first 4080 bytes; `0x01fff000`, a ring of the last
4080) that neither this kernel nor the stock kernel maps, and that survive a
watchdog reset — though not a power cycle. After a trial reverts, boot the
*other* (working) image and read those pages back with `tools/memprobe`
(`tools/memprobe/README.md`) to see exactly how far the failed trial got,
including a stamp confirming U-Boot actually jumped to the new kernel at
all. `docs/FLASHING.md` has the full read-back procedure.

### The previous boot, from our own image

Once both slots run this image, the boot that follows a failed trial is
ours too, and its ramlog would overwrite the failed log at startup. So
before the driver writes anything, it copies both pages (8 KB) into a
static buffer, and the copy stays readable for the whole boot:

    cat /proc/odi_ramlog_prev          # decoded, root only
    cat /proc/odi_ramlog_prev_raw > p  # 8192 bytes: page A, then page B

The decoded file starts with two lines, then the page text the same way
`tools/memprobe/ramlog-read.sh` prints it:

    this boot: boot=8 slot=0
    previous boot: boot=7 slot=1 build=odi-oss-260924-618k1 crumb=TICK/51234 reason=reboot
    ---- page A: first 3984 bytes ----
    ...
    ---- page B: last 4080 of 51234 bytes ----
    ...

It reaches one boot back, and a power cycle still wipes both pages. A
boot of the stock image in between writes nothing, so the "previous boot"
is then the last boot of ours before it; the boot counter and build id
say which one it was.

### The page A boot metadata block

The last 64 bytes of page A (`+4032`) hold one block per boot, written
right after the page headers (`odi_ramlog.h` has the layout):

| offset | field |
|---|---|
| `+0` | magic `RLGM` |
| `+4` | boot counter: the previous value + 1 when the magic was valid, else 1. It counts boots of this image since the last power cycle |
| `+8` | slot, 0 or 1, from the last `root=` on the command line (`31:5` is slot 0, `31:7` slot 1); `0xffffffff` when neither |
| `+12` | block format: 2 (1 before the reset reason block, below) |
| `+16` | build id, 40 bytes NUL padded: `ODI_BUILD_ID` from `kernel/build.sh`, which is the image `VERSION` when one is set for the kernel build, else the `odi-oss-<date>-<rev>` default `image/build.sh` uses |
| `+56`, `+60` | the crumb stash (below) |

Page A text now stops at 3984 bytes, so neither block is ever
overwritten. Readers that take `min(count, 4080)` bytes still read exactly
the text; a page written by an older image (text to 4080 bytes) has no
`RLGM` magic, and one written by a format 1 image (text to 4016 bytes, no
reason block) has format 1.

### The reset reason block

The 32 bytes before the metadata block (`+4000`) record why the boot
ended, so the next one does not have to guess from the free text at the
end of page B. The metadata block keeps its offset, so its crumb stash is
still where `kernel_entry_setup` writes it and a format 1 kernel still
counts boots across ours.

| offset | field |
|---|---|
| `+0` | magic `RLGR` |
| `+4` | reason code, `ODI_RAMLOG_REASON_*` in `odi_ramlog.h` |
| `+8` | detail, 16 bytes NUL padded: the client name for a client miss |
| `+24` | reserved, zero |

`odi_ramlog_meta_stamp()` writes it empty at boot. From then on the last
writer wins: `odi_wdt.c` records the rule that fired (`wdt_client`,
`wdt_mem`, `wdt_userland`) just before it forces the reset, and
`odi_ramlog.c` registers, at `early_initcall`, a reboot notifier
(`reboot`, `halt`, `poweroff`; the reboot syscall path, which an emergency
restart skips), a panic notifier at the highest priority (`panic`) and a
die notifier (`oops`, kernel-mode only: `do_be()` in
`arch/mips/kernel/traps.c` sends every bus error, a user one too, down the
same chain). The next boot renders it as `reason=` at the end of the
`previous boot:` line: the recorded name, `power` when neither page magic
survived, and `unknown` when the pages survived but nothing wrote a
reason. The 6.18 kernel is `ARCH=mips`; none of `kernel/618/patches`
touches `traps.c`, `kernel/panic.c` or `kernel/reboot.c`, so these are the
mainline chains.
Build the kernel with the image version to have them match:
`VERSION=odi-oss-260924-618k1 kernel/build.sh`.

## Early crumbs

For a kernel that dies before its first console line reaches the ramlog,
`CONFIG_ODI_EARLY_CRUMBS` (on in the seed config) stores a 4-byte ASCII
tag and a step number in ramlog page B at `+8`/`+12` as boot passes each
board hook. The last pair written is how far the boot got;
`tools/memprobe/ramlog-read.sh` prints it. The trail, all in our own files:

| tag | step | where |
|---|---|---|
| `K1EN` | 1 | first instructions of `kernel_entry` (`kernel_entry_setup`) |
| `K2SU` | 2 | the CCTL/TCM/cache entry sequence done |
| `K8PB` | 8 | `prom_init()`, argv copied |
| `K9PA` | 9 | end of `prom_init()`; the ramlog console is live from here |
| `KAMS` / `KBCI` | 10 / 11 | `plat_mem_setup()` entered / caches installed |
| `KCIR` / `KDIR` | 12 / 13 | `arch_init_irq()` entered / done |
| `KETM` / `KFTM` | 14 / 15 | `plat_time_init()` entered / done |
| `KGLT` | 16 | `late_time_init`: the one-shot `odi_irqdbg:` dump to the ramlog |
| `IRQE` | count | every interrupt, in a stub ahead of `handle_int` |
| `IRQD` | count | the board interrupt controller dispatch |
| `TICK` | count | the TIMER0 interrupt |

`K1EN` overwrites the pair the previous boot left, so `kernel_entry_setup`
first moves that pair to the crumb stash at page A `+4088`/`+4092`
(`odi_crumb_stash`, eight uncached loads and stores), and the previous-boot
copy puts it back into its saved page B header: `/proc/odi_ramlog_prev`
shows the previous boot's last crumb. Read from the stock image,
`ramlog-read.sh` prints the stash as the crumb of the boot before the one
in page B.

On a healthy boot the pair read back is `TICK` with a large count. From
`K9PA` on, the ramlog text says more than the crumb. The stretch between
`K2SU` and `K8PB` (the end of `head.S`, `start_kernel`, `cpu_probe`) has no
board hook; `CRUMBS_CORE=1 kernel/build.sh` applies
`kernel/618/debug/0001-core-crumbs.patch`, which adds `K3BS`, `K4JS`,
`K5SK`, `K6CB`/`K7CA` and the `KR*` probe steps there. It edits mainline
files and is not part of the normal build.

## Verifying a build

`image/build.sh` disassembles every ELF in the rootfs and fails on an
instruction the RLX5281 does not implement (`packages/isa-audit.sh`,
`packages/isa-allowlist.sh`, see `docs/CROSS-COMPILING.md`). The kernel is
not in the rootfs, so check it by hand after a kernel change:

    packages/isa-audit.sh build/kernel-618/vmlinux    # must find nothing
    tools/kernel-footprint.sh                         # 19 lines, 6 mainline files today

and, with the toolchain's `mips-linux-uclibc-objdump -d` on that
`vmlinux`, count `eret` and `tne`: both must be 0 (the first proves the
R3000 exception model, the second a trap-free `BUG_ON()`).

`kernel/build.sh` ends with `BUILD OK: <n> warnings`. The release build
prints 22, all GCC 16's "'retain' attribute ignored" in mainline
networking files (`net/core/filter.c`, `net/core/xdp.c`,
`net/ipv4/tcp_cong.c`, `net/ipv4/tcp_cubic.c`): a toolchain and mainline
mismatch, not ours. Any other warning is from our files and is a
regression.

`make test-host` (no stick, no kernel build) covers the switch, GPON,
OMCI, NIC, watchdog, ramlog and replay-table logic that has no kernel
dependency, built and run natively against the same sources the kernel
build uses.
