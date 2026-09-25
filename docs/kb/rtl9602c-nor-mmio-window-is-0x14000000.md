# The SPI NOR memory-mapped read window is at physical 0x14000000

The SoC exposes the SPI NOR flash for direct memory-mapped reads at
physical address 0x14000000; reading from an unrelated, undecoded address
nearby stalls the memory bus permanently rather than failing cleanly.

*Last verified: 2026-09-23*

---

## What

With the flash controller in continuous-read mode, flash offset N is
readable directly at physical address `0x14000000 + N`. U-Boot's own saved
environment records the two kernel images' load offsets through this same
window. The SoC only decodes a specific range of physical addresses for
memory-mapped I/O; nothing is decoded at some nearby addresses that look
superficially similar (for example `0x1D000000`), and a load from one of
those stalls the bus forever with no CPU exception raised at all —
interrupts stop being serviced, and only the hardware watchdog can recover
the CPU (see the caveat on that in
[the undecoded-address note](rtl9602c-undecoded-address-read-takes-the-stick-down.md)).

## Evidence

On the stock firmware, reading physical addresses within the documented
window returned exactly the expected magic bytes for the kernel images
stored there. A replacement bring-up that mistakenly read from the
undecoded address above hung on its very first access to that region;
switching to the correct window address let it proceed and mount its root
filesystem normally. The flash controller's own configuration register
confirmed an 8 MB part, 3-byte addressing, single-line I/O.

## See also

- [NOR flash part](rtl9601-nor-flash-part.md)
- [Undecoded address read takes the stick down](rtl9602c-undecoded-address-read-takes-the-stick-down.md)
