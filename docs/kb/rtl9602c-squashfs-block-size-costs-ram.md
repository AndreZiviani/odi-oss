# Squashfs's own in-memory caches cost real RAM on a 32 MB device, and block size drives most of it

Squashfs keeps at least one full data block cached per open file plus a
small pool of fragment-cache blocks, none of it reclaimable; on a 32 MB
device, using large (1 MB) filesystem blocks measurably costs several
megabytes of RAM compared to smaller blocks and a leaner cache
configuration.

*Last verified: 2026-09-25*

---

## What

Squashfs allocates an unreclaimable "data" cache (at least one full block,
unless a specific "direct" read mode is used) plus a fragment cache of
several full blocks by default. Both come from the kernel's general-purpose
allocator and are never freed for the life of the mount. The decompressor
used for this image also preallocates its own working buffer sized to one
block. With a 1 MB squashfs block size, this adds up to several megabytes
of permanently unavailable RAM on a 32 MB device — a meaningful fraction of
total memory on hardware this constrained. Note that the fragment-cache
size is only actually configurable when a specific squashfs kernel option
is enabled; without it, an attempt to reduce the fragment cache size is
silently ignored.

## Evidence

One measured before/after comparison (changing several related settings at
once — direct block reads, the option that makes fragment-cache size
configurable, reducing the fragment cache to a single block, using a
smaller 256 KB block size, and a smaller kernel log buffer) showed
kernel-reported unreclaimable slab memory drop by about 3.8 MB and reported
available memory increase by about 10.9 MB, on the same 32 MB device.
Because several settings changed together, this has not been isolated to
one specific cause — the improvement is real and measured, but the split
between "block size" and "cache tuning" specifically is not proven. This
same combination of settings was, in one round of boot-reliability testing,
an unconfirmed suspect in a couple of otherwise-unexplained init-time
stalls. It was cleared: those stalls were very likely the CPU-port NIC DMA the loader
leaves running, writing received frames into pages the kernel had already
given out (the board code now stops it in `prom_init()`, see
`docs/KERNEL.md`); the image with these squashfs settings has since passed
a 15-boot loop and a 3-hour soak.

## See also

- [Kernel needs R3000 exception model](rtl9602c-rlx5281-kernel-needs-r3000-exception-model.md)
