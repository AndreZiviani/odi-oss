# The optical module sits on I2C port 1, and its pins need an explicit GPIO route

The optical module's I2C bus is routed through the SoC's second I2C port,
and answers NO_ACK on every read until a specific GPIO-mux register is also
set to route the physical pins to that block.

*Last verified: 2026-09-21*

---

## What

The stock firmware configures the second I2C controller (port 1) for the
optical module rather than the first. Even with that port initialised and
enabled, every digital-diagnostics read returned NO_ACK until a separate
GPIO pin-routing register (`IO_GPIO_EN`, offset `0x048`) was also set to
the value the stock firmware uses (`0x08082001`), which is what actually
hands the physical pins over to the I2C block rather than some other
function.

## Why it matters

Some reference designs and configuration baselines for this SoC family
default the optics to the *first* I2C port instead. Building a bring-up
for this specific board needs the second port selected, initialised and
enabled — and, separately, the GPIO mux register set — or every optical
diagnostics read fails with NO_ACK and looks exactly like a dead or
missing module, when the actual cause is a routing register nobody
touched.

## Evidence

Every digital-diagnostics read returned NO_ACK until the GPIO mux register
was set to the value above, at which point reads returned the module's
vendor name string and live optical diagnostics values correctly.

## See also

- [Laser TX-disable is SoC GPIO 13](dfp34x-laser-tx-disable-is-soc-gpio-13.md)
- [DDM scaling (SFF-8472)](rtl9601-ddm-scaling-sff8472.md)
