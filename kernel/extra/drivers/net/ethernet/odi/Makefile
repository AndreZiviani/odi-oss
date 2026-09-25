# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_ODI_NIC) += odi_nic.o
obj-$(CONFIG_ODI_OMCI) += odi_omci.o
obj-$(CONFIG_ODI_SWITCH) += odi_switch.o odi_switch_cmd.o odi_switch_dal.o odi_switch_tbl.o odi_switch_l2.o odi_reg.o odi_i2c.o odi_ddm.o
# odi_replay_blob.o/odi_replay_fw.o -- the register-replay tables (modload
# here, sdkinit and the GPON boot init below) are firmware files under
# /lib/firmware/odi/, not linked data: validated and decoded by
# odi_replay_blob.c (host-testable), loaded with request_firmware() by
# odi_replay_fw.c. ODI_SWITCH selects FW_LOADER and CRC32 for them (Kconfig).
obj-$(CONFIG_ODI_SWITCH) += odi_replay_blob.o odi_replay_fw.o
# odi_switch_sdkinit.o -- "SDK-init replay" -- built only under
# CONFIG_ODI_SDKINIT (Kconfig), which depends on CONFIG_ODI_GPON: the
# /proc/rtk_init dispatch (odi_rtk_init.c) is the only caller, and that
# dispatch is guarded the same way. Its data is /lib/firmware/odi/sdkinit.bin.
obj-$(CONFIG_ODI_SDKINIT) += odi_switch_sdkinit.o
# odi_intr.o -- our own IRQ 8 (apl_sw) demux, under CONFIG_ODI_INTR
# (Kconfig, depends on CONFIG_ODI_SDKINIT). Listed after the ODI_SWITCH
# group above: odi_intr_init() calls odi_reg_read()/odi_reg_write(),
# which need odi_switch_base already mapped (odi_switch_init(), same
# obj-y list, earlier), and built-in module_init() call order follows
# object link order.
obj-$(CONFIG_ODI_INTR) += odi_intr.o
# odi_board.o/odi_board_data.o -- board-init LED/I2C-core replay, under
# CONFIG_ODI_BOARD (Kconfig, depends on CONFIG_ODI_INTR). odi_board_init()
# does not rely on Makefile obj-y order the way odi_intr_init() above
# does: it calls odi_switch_mmio_ensure() (odi_switch.c) itself before any
# odi_reg_write(), because its OWN caller (the board code in
# arch/mips/rtl8686/board.c, a DIFFERENT built-in.o entirely) can run its
# device_initcall before this driver set's own -- see odi_switch.c's own
# odi_switch_mmio_ensure() header comment for the System.map evidence.
obj-$(CONFIG_ODI_BOARD) += odi_board.o odi_board_data.o
# odi_rtk_init.o -- our own from-scratch /proc/rtk_init, under
# CONFIG_ODI_SDK_OUT (Kconfig, depends on CONFIG_ODI_BOARD). Built ONLY
# under ODI_SDK_OUT, replacing the proprietary switch SDK /proc/rtk_init
# handler outright rather than patching it.
obj-$(CONFIG_ODI_SDK_OUT) += odi_rtk_init.o
# odi_wdt.o -- the watchdog kicker, under CONFIG_ODI_WDT (Kconfig, depends
# on CONFIG_ODI_SDK_OUT). Standalone: its own three-address allowlist on
# the SoC system-controller window, no odi_switch_base dependency, so
# unlike odi_intr.o/odi_board.o above it needs no particular position in
# this obj-y list relative to the CONFIG_ODI_SWITCH group.
obj-$(CONFIG_ODI_WDT) += odi_wdt.o
# odi_ramlog.o -- the struct console driver mirroring printk into DRAM,
# under CONFIG_ODI_RAMLOG (Kconfig, standalone -- no dependency on the
# ODI_NIC..ODI_WDT chain above). Listed here, after odi_wdt.o, only to keep
# every entry for a full driver build together; odi_ramlog_console_init() has
# no ordering dependency on any other object in this list (console_
# initcall() runs long before any of their module_init()/device_initcall()
# probes, and odi_ramlog.c touches only its own two fixed DRAM addresses).
obj-$(CONFIG_ODI_RAMLOG) += odi_ramlog.o
# ODI_BUILD_ID (kernel/build.sh, from the environment) is the build id the
# ramlog stamps into each boot metadata block. Only this object carries it,
# so a new id rebuilds one file, not the tree.
ifneq ($(ODI_BUILD_ID),)
CFLAGS_odi_ramlog.o += -DODI_RAMLOG_BUILD_ID=\"$(ODI_BUILD_ID)\"
endif
# odi_i2c.o (SoC I2C indirect-controller primitive) / odi_ddm.o (the seven
# DDM selectors, odi_reg.c's ODI_SW_IOC_DDM_GET) -- built into
# CONFIG_ODI_SWITCH above, not a separate Kconfig symbol: this is
# switch-core-register work odi_switch.c already owns the MMIO window
# for, same reasoning odi_reg.c itself is not its own symbol. Both files
# need it too, so (like odi_switch_sdkinit.c/odi_board.c) they
# #include odi_switch_reg.h themselves; no CFLAGS_*.o force-include line
# needed.
# odi_gpon_stub.o (every call answered -ENODEV, a link and boot check only)
# is gone: the real FSM/PLOAM/register core, host-tested against a
# captured re-activation (tools/regtrace/gpon_replay_compare.py), plus
# the kernel-integration glue (odi_gpon.c, this same pass's own final
# integration cut) now sit behind the unchanged odi_gpon.h interface.
obj-$(CONFIG_ODI_GPON) += odi_gpon.o odi_gpon_hw.o odi_gpon_isr.o odi_gpon_fsm.o \
			   odi_gpon_ploam.o odi_gpon_init.o

# odi_switch_tbl.c/odi_switch_dal.c/odi_gpon_hw.c/odi_gpon_isr.c/
# odi_gpon_init.c call odi_reg_read()/odi_reg_write() (defined in
# odi_switch.c): the host test path gets both from test/odi_switch_mock.h
# static inline definitions, included ahead of each of these files in one
# unity-build translation unit; the real separate-object __KERNEL__ build
# gets them from each file's own `#include "odi_switch_reg.h"` (also where
# odi_switch_dal.c gets linux/printk.h, for its own ODI_SW_STUB_LOG
# pr_info_once() calls) -- no CFLAGS_*.o force-include line needed for any
# of them, the same posture odi_switch_sdkinit.c/odi_board.c already use.

# odi_switch_cmd.c includes the OMCI argument struct headers (omci_gemflow.h,
# omci_bdgconn.h, omci_bridgeport.h) by bare name, from src/omci/ -- ours,
# but outside the kernel tree kbuild copies (kernel/extra only). Bind-mounted
# read-only at /omci by kernel/build.sh; -I here is what makes the bare
# #include resolve.
CFLAGS_odi_switch_cmd.o := -I/omci
