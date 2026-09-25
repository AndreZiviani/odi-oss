# The kernel

Linux **6.18.53**, the current longterm release, built for the RTL9602C.
There is no dual-kernel build any more: `kernel/build.sh` builds 6.18 and
nothing else, from a pristine `cdn.kernel.org` tree with our own patches and
drivers on top — no vendor kernel tree is fetched, patched or linked at any
point in this build.

    kernel/618/fetch.sh    fetches and verifies linux-6.18.53.tar.xz
    kernel/618/mainline/   the pristine tree it extracts, gitignored
    kernel/618/patches/    our edits to mainline files, applied in order
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
an invalidate through the kernel alias. The history: 618h2 failed 4 of 9
boots (init killed right after `Run /sbin/init`) while
`flush_data_cache_page()` only wrote the dcache back; the per-line 0x1b
(c4e1248) took 618h3 to 15/15, which fits stale lines of freed initmem,
filled at the kernel colour. 618n1 (k618 + config diet + plain
CPU_R3000) then failed with that fix in place: busybox `login` took a
store address error (AdES, BadVA 0xffffe0b0) at EPC 0x00421c04, whose
bytes in memory are `jr ra` -- an instruction that cannot store, so the
CPU ran something other than memory. The diet moved the kernel layout and
the allocation pattern; a line left at a user colour by the page's
previous life is the case the per-line op cannot cover. The fix assumes
nothing about the indexing: a whole-cache invalidate is correct for any
of them. Not the cause: an ASID-tagged virtual icache (no flush on ASID
wrap, `cpu_has_vtag_icache` 0) would fail on most boots, since rcS wraps
the 64 ASIDs many times, and a per-page fix would not have taken 4 of 9
failures to 0 of 15.

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
| `odi_gpon*.c` | the GPON MAC block: the O1–O7 activation state machine and PLOAM codec (ITU-T G.984.3), register leaves, the interrupt handler |
| `odi_omci.c` | the netlink transport that carries OMCI frames between the kernel and `omcid` |
| `odi_intr.c` | the shared switch/GPON interrupt line, demultiplexed to the drivers above |
| `odi_board.c` | board-init: LED and I2C core bring-up |
| `odi_i2c.c`, `odi_ddm.c` | the I2C bus to the optical module, and SFF-8472 DDM (temperature, voltage, bias, tx/rx power) readout |
| `odi_reg.c` | `/dev/odi_sw`: register/SoC/MIB access for our own userland (`diag`, `metricsd`) |
| `odi_wdt.c` | the watchdog kicker — see below |
| `odi_ramlog.c` | the DRAM ring-buffer console — see below |

The switch/GPON/OMCI/board pieces are all built statically into `vmlinux`;
there are no loadable kernel modules for the datapath, and none of this
repo's build can produce one — `image/build.sh` stages zero `.ko` files.

## Building it

    make toolchain       # once: gcc/binutils/uClibc-ng for this CPU
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
as `/etc/kernel-config` for reference and for `rcS` to gate platform init on.

## The boot command line and the watchdog

U-Boot passes a per-slot command line in `argv`: which root device
(`root=31:5` for slot 0, `root=31:7` for slot 1), and the rest of what
`prom_init` needs. Nothing in the kernel hardcodes a slot.

U-Boot arms the hardware watchdog before handing over to a **trial** boot
(`nv setenv sw_tryactive <slot>`, see `docs/FLASHING.md`); the kernel's job
is to keep kicking it. `odi_wdt.c` is a small, from-scratch kicker thread:
it arms and kicks at the same operating point U-Boot's own `en_wdt` already
sets, and — critically — it also tracks a **userland confirmation**
deadline, not just "the kernel is alive." A kernel that boots fine but whose
init scripts hang before confirming still gets reset, rather than sitting up
forever on an image nobody can reach. `wdt_pre_reset_hook()` (called from
`odi_nic.c`) quiesces the NIC's DMA engine before a watchdog-triggered reset,
the same way an orderly shutdown would.

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
    previous boot: boot=7 slot=1 build=odi-oss-260924-618k1 crumb=TICK/51234
    ---- page A: first 4016 bytes ----
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
| `+12` | block format, 1 |
| `+16` | build id, 40 bytes NUL padded: `ODI_BUILD_ID` from `kernel/build.sh`, which is the image `VERSION` when one is set for the kernel build, else the `odi-oss-<date>-<rev>` default `image/build.sh` uses |
| `+56`, `+60` | the crumb stash (below) |

Page A text now stops at 4016 bytes, so the block is never overwritten.
Readers that take `min(count, 4080)` bytes still read exactly the text;
a page written by an older image (text to 4080 bytes) has no `RLGM` magic.
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

Every object this build actually compiles is checked, and every ELF that
reaches the image is disassembled and checked again
(`packages/isa-audit.sh`, `packages/isa-allowlist.sh`) for instructions the
RLX5281 does not implement — see `docs/CROSS-COMPILING.md`. `make test`
(host-side, no stick needed) and `make test-host` cover the switch/GPON/OMCI
driver logic that has no kernel dependency, built and run natively against
the same headers the kernel build uses.
