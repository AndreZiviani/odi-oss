# Field notes: ODI DFP-34X-2C2 / RTL9601 / RTL9602C

These are field notes from reverse-engineering this device's behaviour — the
stock (OEM) firmware, the hardware underneath it, and the kernel/toolchain
work that goes into replacing it. Every claim here was validated on real
sticks, not inferred from a datasheet alone. That said: this is reverse
engineering of a closed device by outsiders, not vendor documentation. Some
of it may be wrong, incomplete, or specific to a particular firmware/silicon
revision. Where a finding is a single observation or is known to be specific
to one driver generation, the note says so — read that qualifier before
relying on it.

Corrections and additional data points (especially from other units, other
OLTs, or other firmware revisions) are welcome as issues or PRs.

## Hardware

| file | what it covers |
|---|---|
| [The die and the marketed part number are different names for the same chip](rtl9602c-chip-identity.md) | chip identity: RTL9602C die vs RTL9601D-VA3 marketed part |
| [The flash part is a Macronix-class 8 MB SPI NOR chip](rtl9601-nor-flash-part.md) | flash part identity and driver fallback |
| [The SPI NOR memory-mapped read window is at physical 0x14000000](rtl9602c-nor-mmio-window-is-0x14000000.md) | NOR flash memory-mapped read window address |
| [Reading an undecoded SoC address from the running stock firmware takes the whole unit down](rtl9602c-undecoded-address-read-takes-the-stick-down.md) | undecoded-address bus stall hazard |
| [The RTL9602C register table comes from the stock firmware, not from a published one](rtl9602c-register-table-from-stock-binary.md) | register table revision mismatches |
| [The optical module sits on I2C port 1, and its pins need an explicit GPIO route](dfp34x-optics-on-i2c-port1-gated-by-io-gpio-en.md) | optics I2C routing/GPIO mux |
| [Two different stored LAN SerDes mode values produced the same runtime mode](rtl9601-lan-sds-mode-key.md) | host-side SerDes config key mystery |
| [The host-side SerDes mode lives in one register, and "Fiber 1G" is a write sequence, not a single value](rtl9602c-serdes-mode-encoding.md) | host SerDes register write sequence |

## GPON / optics

| file | what it covers |
|---|---|
| [The module TX-disable line is SoC GPIO 13](dfp34x-laser-tx-disable-is-soc-gpio-13.md) | laser TX-disable pin and its effect on PON sync |
| [The GPON receiver stays in loss-of-signal until one specific analog register bit is set](rtl9602c-gpon-los-needs-rx-sd-por-sel.md) | GPON RX loss-of-signal register requirement |
| [A PON-side IP block must be explicitly powered on before switch-core init, or the bus stalls permanently](rtl9602c-pbo-ip-enable-before-switch-init.md) | PON IP-block power-enable ordering hazard |
| [The stock firmware silently rewrites its own PON mode and reboots after ~24s of unlocked light](rtl9601-pondetect-pon-mode-rewrite.md) | automatic GPON/EPON mode rewrite on sync failure |
| [DDM optical readings follow plain SFF-8472 scaling](rtl9601-ddm-scaling-sff8472.md) | optical diagnostics (DDM) value scaling |

## U-Boot / flash

| file | what it covers |
|---|---|
| [The U-Boot environment is a redundant pair, and its CRC covers everything but the flags byte](rtl9601-uboot-env.md) | U-Boot env layout and CRC quirk |
| [U-Boot passes a per-slot boot command line as argv; a replacement kernel must read it and let it win](rtl9601-uboot-passes-per-slot-cmdline-in-argv.md) | per-slot boot cmdline handoff |
| [U-Boot has a one-shot trial-boot slot; use `sw_tryactive`, not `sw_commit`](rtl9601-uboot-trial-boot.md) | one-shot trial-boot mechanism |
| [Two firmware partitions, so flashing is reversible](rtl9601-dual-firmware-partitions.md) | dual-partition flashing model |
| [A replacement kernel must kick the hardware watchdog at arm, not one interval later](rtl9601-watchdog-kicker.md) | watchdog kicking requirements for a replacement kernel |
| [Two DRAM pages survive a watchdog reset and can carry a kernel log across it](rtl9601-dram-ramlog-console.md) | serial-console-free boot log technique |
| [`/proc/cmdline` is not what U-Boot passed: the stock kernel renames the flash device](rtl9601-mtd-map-name-rewrite.md) | MTD device name rewrite trap |
| [Config lives in an ordinary jffs2 file, and the stock `flash` wrapper has silent no-ops](rtl9601-config-store.md) | persistent config storage and its CLI wrapper's traps |
| [There is no read-only dump of factory defaults, and a backup's key count drifts harmlessly](rtl9601-config-baseline-traps.md) | config backup/defaults gotchas |
| [Size a payload against jffs2's own behaviour, not against raw file size](rtl9601-jffs2-payload-sizing.md) | config partition sizing math |

## Stock firmware behaviour

| file | what it covers |
|---|---|
| [The stock web server only runs because an earlier boot step failed](rtl9601-boa-launched-from-startup-failure-path.md) | web server launch condition |
| [Nothing feeds the random pool early on this device — dropbear can generate host keys from ~27 bits of entropy](rtl9601-boot-entropy-is-27-bits-without-a-seed.md) | boot-time entropy starvation |
| [Stock busybox lacks common applets, and a missing one turns a pipeline silently empty](rtl9601-busybox-missing-applets.md) | missing busybox applets |
| [A modern busybox `mdev -s` names every device by major:minor on a pre-2.6.32 kernel](busybox-mdev-s-needs-devname-uevent.md) | mdev hotplug naming on an old kernel |
| [A modern busybox telnetd cannot serve even one session without Unix98 PTY support](busybox-telnetd-needs-unix98-ptys.md) | telnetd PTY requirement |
| [The stock (OEM) kernel has no devpts, only legacy BSD pseudo-terminals](rtl9601-legacy-bsd-ptys-only.md) | legacy PTY limitation and its SSH implications |
| [On the stock firmware, the OLD LOID/password value wins when it differs from the new one](rtl9601-loid-old-value-wins.md) | LOID/password provisioning precedence |
| [With multi-LAN mode enabled, host-port frames arrive on a VLAN subinterface, not the raw interface](rtl9601-multi-lan-eth0-2-is-the-host-port.md) | host-port traffic landing on a sub-interface |
| [Moving files on and off the stick: it can only listen, and a half-close silently truncates](rtl9601-netcat-file-transfer.md) | file transfer gotchas with a listen-only device |
| [The stock firmware cannot keep time and has no log but the kernel ring buffer](rtl9601-no-clock-no-syslog.md) | missing clock/log facilities |
| [The free boot-script slot on this device is a property of the image, not the platform](rtl9601-rc-script-slots-depend-on-base.md) | boot-hook script slot collisions |
| [A file missing from the firmware image may be a symlink into RAM](rtl9601-rootfs-symlinks-into-var.md) | read-only rootfs symlink layout |
| [The stock web server can't run CGI, and inetd is unusable — serve your own port instead](rtl9601-userland.md) | running your own service on the stock firmware |
| [The stock CLI can spin at 100% CPU if run non-interactively over SSH](rtl9601-vendor-diag-spins-when-stdin-closes.md) | stock CLI hang on piped/non-interactive input |

## OMCI

| file | what it covers |
|---|---|
| [The stock OMCI daemon is eight processes, not eight threads](rtl9601-omci-app-process-tree.md) | OMCI daemon process structure |
| [GEM flows and T-CONTs alone forward nothing: the bridge-connection rule carries it](rtl9601-omci-bridge-connection.md) | the bridge-connection descriptor that actually enables forwarding |
| [The device's capability tables come from one read-only driver query](rtl9601-omci-device-capabilities.md) | device capability query |
| [The OMCI southbound is a single socket option with a dense command space](rtl9601-omci-driver-interface.md) | OMCI-to-hardware transport and its behavioral traps |
| [Reading a G.988 extended-VLAN tagging entry: the single added tag comes from the inner treatment word](rtl9601-omci-extvlan-table.md) | extended VLAN tagging table (class 171) layout |
| [The firmware's feature bitmasks are fully decodable from the shipped image](rtl9601-omci-feature-bitmasks.md) | feature bitmask decoding |
| [The CLI talks to the OMCI daemon over a System V message queue](rtl9601-omci-ipc.md) | OMCI CLI-to-daemon IPC protocol |
| [The stock firmware's OMCI information model is a set of independently loadable plugins](rtl9601-omci-mib-model.md) | OMCI managed-information model structure |
| [The CLI exposes the OLT's own provisioned view of the MIB, in a format that shifts between tables](rtl9601-omci-mib-readback.md) | MIB readback via the stock CLI |
| [An ONU that reports plausible attribute values gets fully provisioned; one reporting zeros does not](rtl9601-omci-olt-provisions-only-against-vendor-attribute-values.md) | OLT provisioning behaviour vs. reported attribute values (single OLT observed) |
| [An OMCI config change applies without a reboot by restarting the daemon](rtl9601-omci-reapply-without-reboot.md) | applying OMCI config changes without a reboot |
| [OMCI table attributes on this firmware: an off-by-one index and a chunked Get-Next walk](rtl9601-omci-table-attributes.md) | attribute indexing and table-attribute retrieval |
| [A "TPType 3" bridge port can point at either the 802.1p mapper or the GEM interworking termination point, depending on the OLT](rtl9601-omci-tptype3-bridge-port-points-at-mapper-or-iwtp.md) | OLT-dependent TPType 3 interpretation |
| [A VEIP-only bridge connection needs the UNI ports in its mask too](rtl9601-veip-rule-needs-the-uni-ports-in-its-mask.md) | VEIP connection mask gotcha (intermediate driver generation) |
| [Reserve only the queues a GEM CTP actually references, not every queue the OLT sets](rtl9602c-omci-active-queues-are-gem-referenced-only.md) | queue reservation gotcha (intermediate driver generation) |
| [A GEM flow's upstream queue field is a dense per-T-CONT ordinal, not a queue's own id](rtl9602c-omci-upstream-queue-ordinal-is-dense-per-tcont.md) | queue-ordinal encoding (intermediate driver generation) |

## Switch / VLAN

| file | what it covers |
|---|---|
| [The stock CLI's `all` port keyword is a fixed device port class, not a live probe](rtl9601-diag-port-all-semantics.md) | port enumeration semantics in the stock CLI |
| [Only the switch MIB counters show whether the ONU is actually forwarding](rtl9601-forwarding-visibility.md) | which counters actually show forwarding activity |
| [IGMP/MLD control frames reach userland over the same redirect mechanism as OMCI, with a one-based port field](rtl9601-igmp-redirect-path.md) | IGMP/MLD control-frame delivery path |
| [The service VLAN tag lives in the config store, not in the OMCI extended-VLAN table](rtl9601-onu-manual-vlan.md) | where the actual service VLAN tag comes from |
| [An 802.1p-mapped service carries its p-bit in the out-style tag too, not just the filter](rtl9601-vlan-rule-out-style-carries-the-pbit.md) | p-bit placement in 802.1p-mapped VLAN rules |
| [Register access on the stock firmware is getsockopt/setsockopt on a raw socket, not ioctl](rtl9601-register-access-via-sockopt.md) | switch/optics register access transport |

## Kernel port

| file | what it covers |
|---|---|
| [A new MIPS cache implementation must generate clear_page/copy_page itself](mips-cache-init-must-build-clear-page.md) | MIPS cache-init porting requirement |
| [MIPS o32 differs from the generic Linux syscall ABI in several places, and each fails silently](mips-o32-syscall-abi-traps.md) | MIPS o32 syscall ABI traps |
| [RLX5281 instruction set: verify by executing, not by ISA level](rtl9601-isa-map.md) | measured RLX5281 instruction-set support |
| [A kernel port for this CPU must take the R3000 exception model, not R4000's](rtl9602c-rlx5281-kernel-needs-r3000-exception-model.md) | MIPS exception-model Kconfig requirement |
| [Squashfs's own in-memory caches cost real RAM on a 32 MB device, and block size drives most of it](rtl9602c-squashfs-block-size-costs-ram.md) | squashfs RAM cost on a memory-constrained device |

## Tooling / toolchain

| file | what it covers |
|---|---|
| [Decision: build a current gcc + uClibc-ng toolchain targeting mips2, not mips1](rtl9601-build-own-toolchain.md) | current from-source toolchain decision and rationale |
| [Decision: read optical/switch metrics by scripting the stock CLI, not by linking the vendor library directly](rtl9601-cli-over-vendor-lib.md) | why the stock CLI is scripted rather than the vendor library linked |
| [Decision: keep every device-side tool freestanding, not linked against a libc](rtl9601-freestanding-over-libc.md) | freestanding-over-libc rationale |
| [A stock big-endian MIPS GCC builds working freestanding binaries, but only up to -march=mips2](rtl9601-stock-gcc-cross-compile.md) | what a stock distro GCC can and can't build for this core |
| [Historical: a prebuilt vendor toolchain's measured limits, kept for the record](rtl9601-vendor-prebuilt-toolchain.md) | vendor prebuilt toolchain's measured ceilings (superseded) |

---

*Index generated from the notes above; if you rename or add a note, update this table.*
