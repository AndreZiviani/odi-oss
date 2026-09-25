// SPDX-License-Identifier: GPL-2.0
/*
 * odi_board_data.c -- odi_board.h's own odi_board_init_events[]: the 88-entry
 * ordered write replay for CONFIG_ODI_BOARD, HAND TRANSCRIBED (not
 * machine-generated -- fixed size, one boot window, no per-verb or
 * per-mask structure worth a generator the way the modload/sdkinit
 * firmware tables need one; needed at board init, before any filesystem,
 * so it stays built in) from
 * isp1-260923-g4-boot.txt's own "== 2.94 boot" window, lines 3-90 (the
 * regtrace header on line 2 reads "total=88 dropped=0 entries=88"), one
 * struct odi_sw_modload_event row per capture line, IN CAPTURE ORDER --
 * do not reorder or coalesce rows: odi_board_init() (odi_board.c) applies
 * this array exactly as given, and the order across the REG/SOC boundary
 * (rows 85-87, the two board-init SoC writes) is itself part of what this
 * replay is reproducing (odi_board.h's own header comment has the I2C
 * ordering argument).
 *
 * category/reg_group/n_words/table/words are always 0 -- every row here is
 * either ODI_SW_MODLOAD_REG (a switch-core MMIO offset, applied through
 * odi_reg_write()) or ODI_SW_MODLOAD_SOC (a raw KSEG1 physical address,
 * applied through odi_switch_soc_write(), the sdkinit allowlist mechanism)
 * -- this capture window has no ODI_SW_MODLOAD_TABLE row.
 */
#include "odi_board.h"

const struct odi_sw_modload_event odi_board_init_events[] = {
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0000001c, .value = 0x00010000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0000001c, .value = 0x00010001 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0000001c, .value = 0x00010002 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023004, .value = 0x02200031 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023004, .value = 0x022001df },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023018, .value = 0x01022040 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023004, .value = 0x023401df },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x000000b8, .value = 0x00000028 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x000000c0, .value = 0x00000001 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023018, .value = 0x01020040 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023004, .value = 0x0234013a },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023018, .value = 0x01022040 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e000, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e000, .value = 0x00000000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00010000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00008000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e05c, .value = 0x00010008 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x00023014, .value = 0x00008004 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010400 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010600 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010700 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010700 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010700 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010720 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010730 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010738 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010738 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010738 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e040, .value = 0x00010738 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020000 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020800 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020c00 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020e00 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f00 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f00 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f40 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f60 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f70 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f78 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f78 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f78 },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0001e00c, .value = 0x00020f78 },
	{ .kind = ODI_SW_MODLOAD_SOC, .offset = 0xb8000044, .value = 0x0000035f },
	{ .kind = ODI_SW_MODLOAD_SOC, .offset = 0xb8003324, .value = 0x00000a00 },
	{ .kind = ODI_SW_MODLOAD_SOC, .offset = 0xb8003328, .value = 0x000035ed },
	{ .kind = ODI_SW_MODLOAD_REG, .offset = 0x0000004c, .value = 0x00000a00 },
};

const unsigned int odi_board_init_event_count =
	sizeof(odi_board_init_events) / sizeof(odi_board_init_events[0]);
