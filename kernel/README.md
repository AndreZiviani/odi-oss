# The kernel

This directory builds **Linux 6.18** for the RTL9602C, and only that — there
is no other kernel line in this repository. See `docs/KERNEL.md` for the
full detail: the split between patches and the overlay, the CPU and board,
IRQ/timer/NOR handling, our own driver directory
(`kernel/extra/drivers/net/ethernet/odi/`), how the boot command
line and the watchdog work, and the DRAM ramlog console.

    kernel/618/fetch.sh    fetch and verify linux-6.18.53.tar.xz
    kernel/618/mainline/   the pristine tree it extracts (gitignored)
    kernel/618/patches/    our edits to mainline files, applied in order
    kernel/618/debug/      debug-only patches, applied with CRUMBS_CORE=1
    kernel/extra/          every file of our own, copied over the tree at its path
    kernel/618/config      the seed config
    kernel/build.sh        builds it all, in a container, with toolchain/

    make kernel            # from the repo root; needs `make toolchain` first
