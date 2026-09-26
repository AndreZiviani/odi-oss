# The GPON receiver stays in loss-of-signal until one specific analog register bit is set

A single register bit in the PON SerDes analog block (bit 15 of the analog
control register at offset 0x225b0) must be set explicitly, or the
receiver reports loss-of-signal even with every other GPON register
apparently configured correctly.

*Last verified: 2026-09-21*

---

## What

With every other GPON-related register matching a known-working
configuration, a from-scratch bring-up still reported loss-of-signal (LOS).
The one difference, found by comparing the full PON SerDes analog register
block against a working reference, was bit 15 of the analog control
register at offset `0x225b0`. Setting that bit to 1 cleared LOS immediately,
and the unit progressed from its earliest optical state through to full
optical synchronization. Nothing in the stock firmware's own boot path sets
this bit for this specific chip/board combination — it must be set
explicitly during bring-up on top of everything else.

## Why it matters

Anyone bringing the GPON MAC up from scratch on this hardware will stall
here with every other piece of state looking correct — this bit is easy to
overlook precisely because it sits in an analog configuration block that
otherwise needs no attention.

## Evidence

A full register dump of the PON SerDes analog block from a known-working
reference differed from a from-scratch bring-up at exactly this one bit;
writing it cleared loss-of-signal within about a second.

## See also

- [PBO IP enable before switch init](rtl9602c-pbo-ip-enable-before-switch-init.md)
