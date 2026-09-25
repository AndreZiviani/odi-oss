# kernel/extra

Every file this port adds to the kernel, at its path in the tree.
`kernel/build.sh` copies each one (all but the `README.md` files) to the
same relative path in its build copy of `kernel/618/mainline/` (in a Docker
volume; the fetched tree itself stays pristine), after
`kernel/618/patches/` are applied. The patches hold only the edits to
files mainline already has; `docs/KERNEL.md` explains the split.

    arch/mips/rtl8686/                    board file, RLX5281 caches (cache.c),
                                          interrupt controller (irq.c), TIMER0
                                          clock event (time.c), the IRQE crumb
                                          stub (crumb-int.S), Kconfig,
                                          Makefile, Platform
    arch/mips/include/asm/mach-rtl8686/   kernel entry, feature overrides, IRQ
                                          numbering, register map, CCTL and
                                          cache geometry, the sync barrier
                                          (rtl8686-barrier.h), early crumbs
    drivers/mtd/devices/rtl8686-spiflash.c  SPI NOR controller
    drivers/net/ethernet/odi/             switch, GPON MAC, CPU-port NIC, OMCI
                                          transport, board init, register
                                          replays (loaded as firmware from
                                          /lib/firmware/odi), watchdog kicker,
                                          DRAM ramlog

`docs/KERNEL.md` describes what each driver drives; the odi directory's
own `README.md` has the engineering detail.

Adding or removing a file here changes the patch stamp, so the next build
re-extracts the tree; an edit to an existing file rebuilds incrementally.

All of it is our own code, GPL-2.0, an independent implementation for this
board — see `docs/LICENSING.md`.
