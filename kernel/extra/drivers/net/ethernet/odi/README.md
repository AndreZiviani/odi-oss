# odi_nic -- open CPU-port NIC driver

GPL-2.0, written from scratch from observed register-level behaviour.
It replaces the closed-source CPU-port NIC driver for the RTL9602C: the
DMA engine that moves frames between host memory and the CPU port on the
switch.

Built by `kernel/build.sh` as `CONFIG_ODI_NIC=y`, part of the standard
driver set -- no proprietary NIC driver is ever built on this tree.

`drivers/net/ethernet/Makefile` visits this directory unconditionally
(`obj-y += odi/`, one line from `kernel/618/patches/0005`); the Kconfig
and Makefile here decide what builds, from `ODI_NIC` down the dependency
chain to `ODI_WDT`.

Proven on hardware 2026-09-21: all five
acceptance criteria passed over this driver -- management on `br0`, IRQ 26,
`gpon_onu_state 5`, `gpon_omci_services 6`, and an Internet path through
the stick. See `docs/IMPROVEMENTS.md` for what remains undecided.

Scope: a bridge ONU with management on `br0` -- no multi-WAN, no
software bridge fast-path (redundant with the kernel bridge already in
use), no external-switch support, no hardware LSO, no descriptors in
SRAM.

## The interface to odi_omci

Besides its net_devices, `odi_nic.c` exports two entry points, both used
only by `odi_omci.c`:

- `int odi_nic_rxhook_register(int portmask, int priority, odi_rxhook_fn rx)`
  / `odi_nic_rxhook_unregister` (same signature) -- a callback for a
  port-mask/priority slice of RX traffic, so OMCI frames reach `omcid`
  without a net_device of their own. `odi_rxhook_fn` is
  `int (*)(struct sk_buff *skb, const struct odi_rx_words *rx)`; the
  words are the four RX descriptor words (reason, address, opts2, opts3).
  The NAPI poll walks the registered hooks from the highest priority down
  and calls each one before falling through to `netif_receive_skb`. A
  hook returns `ODI_RXHOOK_STOP`, `CONTINUE` or `STOP_NOFREE`.
- `int odi_nic_tx_words(const void *frame, unsigned short len,
  const struct odi_tx_words *tx)` -- sends a raw frame with the
  destination-port, stream and CPU-tag fields the caller put in the TX
  descriptor words, on the shared TX ring, the same as `ndo_start_xmit`
  does for a netdev-originated frame. OMCI replies go out this way.

## Internal shape

Rings, NAPI (`odi_poll`, standard budgeted polling), and a periodic state dump (`odi_state_dump_work`, counters at
15 s and 30 s) that gets the failure mode of a trial into the RAM log without
needing a live shell on the stick. Hardware is enabled last: IRQ unmask
and the RUN go bit are written only after `request_irq()` and
`napi_enable()` have both succeeded, so no interrupt can land with no
handler installed or a disabled NAPI context -- trial n4 found the
original ordering bug on hardware.
`odi_quiesce_hw()` is the shared teardown -- masks interrupts, stops the
ring engine -- called from both the `ndo_stop`/remove path of the module
itself and from `wdt_pre_reset_hook`, so a watchdog-triggered reset
quiesces the hardware the same way an orderly unload does.

Host-testable core: `odi_nic_hw.h` (register offsets, descriptor layout,
CPU-tag pack/unpack, ring index arithmetic) has no kernel dependency and is
exercised by `test/odi_nic_hw_test.c` with the host `cc`
(`test/odi_nic_hw_test.sh`, part of `make test-host`).

## Lessons from twelve trial-fixing gaps (n1-n17)

Every one of the twelve gaps closed between the first trial and the
passing one (DMA device handling, a KSEG1 address into `ioremap`, IRQ/NAPI
enable ordering, RX ring-size and interrupt-status registers, RX filter mode,
CPU-tag size codes, the RX 2-byte offset and FCS, the TX words as the
whole TX descriptor rather than two ints, the return type of the RX hook
and the RX words/priority-walk/`opts3` fields, the argument order of the
raw TX call, and the missing hardware FCS/CPU tag on the raw TX path) was
a place where a plausible-looking signature or register layout compiled
and linked but did not match what the hardware and the OMCI path
actually need. Each was found on a hardware trial.

## odi_omci -- the OMCI frame transport

`odi_omci.c` (`CONFIG_ODI_OMCI`, depends on `CONFIG_ODI_NIC`) is a second,
separate driver in this directory: a `NETLINK_USERSOCK` kernel socket
speaking the same wire protocol `omcid` already implements
(`src/omci/nl.h`), replacing the proprietary OMCI packet-redirect module.
Our own implementation; the pack/unpack logic
is factored into `odi_omci_wire.h`, host-testable the same way
`odi_nic_hw.h` is (`test/odi_omci_test.c`, part of `make test-host`).

It consumes RX frames ahead of any other hook (an
`odi_nic_rxhook_register()` registration at the highest priority, using
the priority-ordered dispatch in `odi_nic.c`) and sends replies with
`odi_nic_tx_words()` directly. Part of the standard `kernel/build.sh`
driver set.
