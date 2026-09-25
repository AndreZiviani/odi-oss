# The flash part is a Macronix-class 8 MB SPI NOR chip

The device's flash is an 8 MB SPI NOR chip identifying as JEDEC `C22017`
(Macronix, MX25L64-class), driven by the stock firmware through a generic
SPI NOR fallback profile rather than a part-specific driver.

*Last verified: 2026-09-15*

---

## What

Read from the boot log shortly after a reboot, the flash controller
reports an 8 MB device and identifies it by its JEDEC ID as `C2 20 17` —
Macronix, MX25L64-class, 8 MB (the capacity byte alone gives 2^23 bytes).

The boot log also shows which code path drives it: the driver has a small
table of known vendor IDs it recognises by name, and this part is not one
of them, so it silently falls back to a **generic SPI NOR profile** — read
opcode `0x03`, page program `0x02`, sector erase `0x20`, 3-byte addressing,
size taken from the JEDEC capacity byte. This generic-profile fallback is
not a degraded or unusual path: it is the same code path the stock
firmware's own flash driver uses for this exact part, since this part
simply isn't in that driver's small table of specifically-recognised
vendor IDs either. In other words, whatever revision of this NOR driver you
build against, this specific flash part very likely takes the same generic
fallback path — which is a point in favour of building your own driver
against it, since the behaviour has already been exercised by every write
the stock firmware has ever made to this flash: every configuration write,
every SSH host key, every firmware update.

## Why it matters

This board's flash is **NOR**, not NAND — worth stating plainly, since some
generic reference configurations for this SoC family assume a NAND board
and need adjusting (a NOR-appropriate driver, registered under the name the
platform code expects) to see this board's own flash at all.

The boot line identifying which path drove the flash rotates out of the
kernel's in-memory log ring buffer within a few days of uptime (normal
system chatter overwrites it), so it has to be read soon after a boot if
you want to confirm it. The stock image ships no log-reading tool of its
own; a minimal one that reads the kernel ring buffer non-destructively is
enough.

## See also

- [Trial boot](rtl9601-uboot-trial-boot.md)
- [NOR MMIO window](rtl9602c-nor-mmio-window-is-0x14000000.md)
- [MTD map name rewrite](rtl9601-mtd-map-name-rewrite.md)
