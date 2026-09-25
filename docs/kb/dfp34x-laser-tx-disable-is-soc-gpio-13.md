# The module TX-disable line is SoC GPIO 13

The optical module's TX-disable pin is wired to SoC GPIO 13; it comes up
undriven (pulled high by the module), and while high the unit still
achieves PON sync but transmits no light, so it never gets assigned an
ONU-ID and looks unseen from the network side.

*Last verified: 2026-09-21*

---

## What

After optical sync, a unit can stay unseen by the head-end with zero
measured transmit power. GPIO 13 on the SoC is the transceiver's
TX_DISABLE line (direction register `0xb8003308` bit 13, data register
`0xb800330c` bit 13; low means laser on): configuring it as an output and
driving it low brings up transmit power, and the unit is then recognised
by the head end. The transceiver's own digital diagnostics also mirror
this line's state as a readable status bit.

## Why it matters

Nothing in the stock firmware's early boot path sets this pin — the stock
firmware's own userland does it later, during its own bring-up sequence.
Without it, everything else in a from-scratch datapath bring-up can look
correct on every register, while the unit still never leaves its earliest
optical state: with TX_DISABLE left high (undriven, held high by a pull-up
inside the module), the unit's receiver works fine — it syncs to the
downstream signal and even answers serial-number discovery requests — but
because it transmits no light upstream, the head end never assigns it an
ONU identifier, so it never reaches a fully-registered state. This looks
exactly like "unseen" from the network side even though receive is
completely healthy, so it is easy to misdiagnose as an optical or GPON-MAC
problem rather than a single GPIO.

Note that "powers up high" is itself an inference from the pin being
undriven and pulled high by the module, not something measured directly at
power-on.

## Evidence

Configuring the pin as output and driving it low took measured transmit
power from effectively nothing to a healthy positive value, and the unit
was fully recognised by the network shortly afterward.

## See also

- [Optics I2C gating](dfp34x-optics-on-i2c-port1-gated-by-io-gpio-en.md)
