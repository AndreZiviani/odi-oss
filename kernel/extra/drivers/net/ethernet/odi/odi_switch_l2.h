/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The L2 lookup table (LUT): read a row back, walk the valid rows, add and
 * delete L2 multicast entries.
 *
 * The LUT shares the indirect table engine of odi_switch_tbl.c (TABLE_CMD,
 * TABLE_STATUS, TABLE_WRITE_WORD, TABLE_READ_WORD at 0x012000) under
 * TABLE_KIND 0, and uses two things the other tables do not: an access
 * method other than "by row" (a write can be placed by the hardware hash
 * of its key), and the status word the engine leaves behind (whether the
 * key was found, and which row it landed on). odi_switch_table_write()
 * always sends METHOD 1 and never reads the status back, so this file
 * drives the same registers itself rather than bending that primitive.
 *
 * Plain C with no kernel dependency, like odi_switch_tbl.c: the host test
 * (test/odi_switch_l2_test.c) builds it against test/odi_switch_mock.h and
 * a small LUT model there.
 *
 * Every function that touches the engine must be called with
 * odi_switch_lock held (odi_switch.c has the lock design); the /dev/odi_sw
 * ioctls in odi_reg.c take it around each call.
 *
 * ---- Row layout -------------------------------------------------------
 *
 * A row is 78 bits in three 32-bit words. Hardware bit b is bit (b % 32)
 * of TABLE_READ_WORD[b / 32] (and of TABLE_WRITE_WORD[b / 32]): word 0
 * holds bits 0..31. Rows here are always kept in that order, raw[0] first,
 * with no reversal anywhere.
 *
 *   every row     0..47  MAC address, octet 0 in bits 47..40
 *                48..59  the 12-bit key next to the MAC: on a unicast
 *                        row the VID of the learned frame, on a
 *                        multicast row the VID when IVL is set and the
 *                        filtering id otherwise
 *                    61  L3 lookup: the row is an IPv4 multicast route
 *                        (group address in 0..27, no MAC)
 *                    62  not learned: a static row, never aged
 *                    63  IVL: keyed on VID rather than filtering id
 *                    77  valid, on a write only (see below)
 *   unicast          60  FID
 *   (MAC bit 40      64  C-tag present on the learned frame
 *    clear)      65..66  source port
 *                67..69  age, counting down; 0 is aged out
 *                    70  802.1X authorised
 *                    71  block as a source address
 *                    72  block as a destination address
 *                    73  used by ARP
 *                74..76  extension (CPU sub-)port of the source
 *   multicast    66..69  member ports 0..3
 *   (MAC bit 40  70..76  member extension ports 0..6
 *    set, or L3)
 *
 * ---- Which rows are in use -------------------------------------------
 *
 * Not bit 77. Read back by row number, every row of the table has bit 77
 * set, the empty ones included, so it says nothing on a read; it is only
 * the valid flag a write sends (set to place an entry, clear to remove
 * one). What tells a used row from an empty one is TABLE_STATUS.HIT after
 * the read: set for a row that holds an entry, clear for an empty one,
 * with TABLE_STATUS.ROW echoing the row read either way. Checked on a
 * stick with four learned MACs: HIT was set on exactly those four rows
 * of 1024 (docs/KERNEL.md has the readout).
 *
 * An empty row does not read back as zeros either: word 2 is 0x00002000
 * (bit 77 alone), word 1 is zero and word 0 is the row bucket (row / 4),
 * which lands in the low MAC octet. The likely reason is that the table
 * does not store the part of the MAC its bucket already implies and
 * rebuilds it on the way out, so an empty row comes back with that part
 * rebuilt from its bucket alone. Either way, an empty row decodes as a
 * plausible unicast MAC 00:00:00:00:00:NN, and only HIT says it is not.
 *
 * The table has 1024 hashed rows (four ways per bucket) and 64 CAM rows
 * after them; L2_LOOKUP_SETUP.CAM_OFF switches the CAM rows off, and the
 * walk below then stops at 1024. ISP1 reads 0x00400bb8 there,
 * CAM on, so the walk covers 1088; its CAM rows read back as leftover
 * words with bit 77 clear and HIT clear, TABLE_STATUS.IN_CAM set and the
 * CAM row number in ROW.
 *
 * Ports: 0 is the UNI (the SFP host side), 2 the PON, 3 the CPU; 1 is not
 * wired on this board.
 */
#ifndef ODI_SWITCH_L2_H
#define ODI_SWITCH_L2_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* ---- Registers this file needs beyond odi_switch_hw.h ----------------- */

/* Names are regnames.txt ones, like odi_switch_hw.h.
 *
 * TABLE_CMD.WALK_PORT (bits 7..8), the one TABLE_CMD field odi_switch_hw.h
 * does not name: a source port for a per-port walk, unused here and always
 * written 0.
 */
#define ODI_SW_TABLE_CMD_WALK_PORT_MASK	(0x3U << 7)

/* TABLE_STATUS (0x012004), read after the engine goes idle:
 *   13     IN_PROGRESS (odi_switch_hw.h)
 *   12     HIT: a hash access found (read) or placed (write) its key
 *   10     IN_CAM: the row is a CAM row
 *   0..9   ROW: the row within its part (hashed or CAM)
 */
#define ODI_SW_TABLE_STATUS_HIT		(1U << 12)
#define ODI_SW_TABLE_STATUS_IN_CAM	(1U << 10)
#define ODI_SW_TABLE_STATUS_ROW_MASK	0x3ffU

/* TABLE_CMD.METHOD values the LUT understands. Only these two are used:
 * the hardware also offers "next valid row" walks, which this driver does
 * not rely on -- the walk is done in software over METHOD_ROW reads, so
 * nothing depends on how the hardware wraps or skips.
 */
#define ODI_SW_L2_METHOD_HASH		0U	/* place (write) or look up (read) by MAC + key */
#define ODI_SW_L2_METHOD_ROW		1U	/* by row number */

#define ODI_SW_L2_TABLE_KIND		0U

/* L2_LOOKUP_SETUP (0x017000): IPMC_ON_GROUP (bit 23) selects how IPv4
 * multicast data is looked up (0: on the destination MAC and VID/FID, the
 * only mode the MAC-keyed entries below serve; 1: on the group address),
 * CAM_OFF (bit 21) switches the 64 CAM rows off. The stock init was read
 * as writing 0x00600bb8 (CAM off, lookup on MAC + VID/FID); ISP1
 * reads 0x00400bb8, the same lookup mode with the CAM on.
 */
#define ODI_SW_L2_LOOKUP_SETUP_OFF		0x17000U
#define ODI_SW_L2_LOOKUP_SETUP_IPMC_ON_GROUP	(1U << 23)
#define ODI_SW_L2_LOOKUP_SETUP_CAM_OFF		(1U << 21)

#define ODI_SW_L2_HASH_ROWS		1024U
#define ODI_SW_L2_CAM_ROWS		64U
#define ODI_SW_L2_WORDS			3U

/* ---- Decoded row ---------------------------------------------------- */

enum odi_sw_l2_type {
	ODI_SW_L2_UCAST = 0,	/* learned or static unicast */
	ODI_SW_L2_MCAST = 1,	/* L2 multicast, keyed on the MAC */
	ODI_SW_L2_IPMC  = 2,	/* IPv4 multicast route, keyed on the group */
};

/* flags */
#define ODI_SW_L2_F_VALID	(1U << 0)	/* TABLE_STATUS.HIT on the read, not a row bit */
#define ODI_SW_L2_F_STATIC	(1U << 1)	/* not learned, never aged */
#define ODI_SW_L2_F_IVL		(1U << 2)	/* key is a VID, not a FID */
#define ODI_SW_L2_F_CTAG	(1U << 3)	/* unicast: learned from a tagged frame */
#define ODI_SW_L2_F_AUTH	(1U << 4)
#define ODI_SW_L2_F_SA_BLOCK	(1U << 5)
#define ODI_SW_L2_F_DA_BLOCK	(1U << 6)
#define ODI_SW_L2_F_ARP		(1U << 7)

struct odi_sw_l2_row {
	uint32_t index;		/* row number, 0..1087 */
	uint32_t raw[ODI_SW_L2_WORDS];
	uint8_t  mac[6];	/* zero for an IPv4 multicast route */
	uint16_t key;		/* unicast: learned VID; multicast: VID (IVL) or FID (SVL) */
	uint8_t  type;		/* enum odi_sw_l2_type */
	uint8_t  port;		/* unicast: source port */
	uint8_t  ext_port;	/* unicast: source extension port */
	uint8_t  age;		/* unicast: 0..7 */
	uint32_t flags;		/* ODI_SW_L2_F_* */
	uint32_t fid;		/* unicast: the FID bit */
	uint32_t ports;		/* multicast: member ports 0..3 */
	uint32_t ext_ports;	/* multicast: member extension ports 0..6 */
	uint32_t group;		/* IPv4 route: low 28 bits of the group */
};

/* A multicast entry to add or delete. The key is (mac, key, ivl); the
 * member masks are ignored by a delete.
 */
struct odi_sw_l2_mcast_req {
	uint8_t  mac[6];
	uint16_t key;
	uint32_t ivl;		/* 1: key is a VID; 0: key is a filtering id */
	uint32_t ports;		/* bits 0..3 */
	uint32_t ext_ports;	/* bits 0..6 */
};

/* Pure layout: no register access, host-tested on their own. The decode
 * never sets ODI_SW_L2_F_VALID: whether a row is in use is not in its
 * bits (see "Which rows are in use" above), and odi_switch_l2_read()
 * adds it from the status word.
 */
void odi_switch_l2_decode(struct odi_sw_l2_row *row);
int odi_switch_l2_mcast_encode(const struct odi_sw_l2_mcast_req *req, int valid,
			       uint32_t raw[ODI_SW_L2_WORDS]);

/* A multicast MAC this driver will write: the group bit set, and neither
 * the broadcast address nor the reserved 01:80:c2:00:00:0x block, whose
 * frames the switch handles through its own trap rules -- an LUT row for
 * one of those would silently change where ARP or a BPDU goes.
 */
int odi_switch_l2_mcast_mac_ok(const uint8_t mac[6]);

/* Rows the walk covers: 1024, or 1088 with the CAM rows on. */
uint32_t odi_switch_l2_rows(void);

/* L2_LOOKUP_SETUP.IPMC_ON_GROUP, as 0 or 1. */
uint32_t odi_switch_l2_ipmc_mode(void);

/* One row by number, valid or not; ODI_SW_L2_F_VALID set when
 * TABLE_STATUS.HIT says the row holds an entry. 0, -EINVAL-style -22 for a row past
 * the table (a CAM row counts as past it while the CAM is off), -1 when
 * the engine stays busy.
 */
int odi_switch_l2_read(uint32_t index, struct odi_sw_l2_row *row);

/* The first valid row at or after *index, into *row (and *index). 0 when
 * one is found, -2 (-ENOENT) when there is none left, -1 on a busy engine.
 */
int odi_switch_l2_next(uint32_t *index, struct odi_sw_l2_row *row);
#define ODI_SW_L2_NEXT_NONE	(-2)	/* odi_switch_l2_next(): no row left */

/* Place a multicast entry through the hardware hash, or remove the entry
 * with the same key. add: 0 and *index = the row it landed on; -28
 * (-ENOSPC) when every way of its bucket is taken; -22 for a MAC
 * odi_switch_l2_mcast_mac_ok() refuses or a mask with bits past the
 * ports; -1 on a busy engine. del: 0 whether or not the key was present
 * (*found says which, when non-NULL); the key is looked up through the
 * hash first and nothing is written when it is absent.
 */
int odi_switch_l2_mcast_add(const struct odi_sw_l2_mcast_req *req, uint32_t *index);
int odi_switch_l2_mcast_del(const struct odi_sw_l2_mcast_req *req, int *found);

/* ---- Static unicast rows: the stick's own MACs on the CPU port ----------
 *
 * A learned row moves to whatever port last sent a frame with its MAC as
 * the source, the PON port included: one frame from the fibre carrying the
 * stick's MAC would divert host-to-stick traffic upstream. A static row
 * (bit 62, the vendor's "no SA learning") is not learned over: a frame
 * whose source hits it from another port is an "illegal port move",
 * handled by SA_MOVED (0x01c000, two bits a port: 0 moves a learned row to
 * the new port, 1 drops the frame and leaves the row; see
 * odi_switch_init_modload() for the PON port, where it is 1). Age is
 * written non-zero, as the vendor SDK does for a static unicast row. The key is the one the
 * hardware gives the stick's MAC when it learns it from the CPU port (SVL,
 * filtering id 0, VID 0: docs/KERNEL.md), so the write lands on the
 * learned row and replaces it. docs/SWITCH.md#the-sticks-mac has why.
 */

/* A MAC this driver will pin: unicast and not all zero. */
int odi_switch_l2_ucast_mac_ok(const uint8_t mac[6]);

/* The row of a static unicast entry for mac on port, valid set; with
 * valid 0 the key alone (MAC, key 0, SVL), for a delete. -22 for a MAC
 * odi_switch_l2_ucast_mac_ok() refuses or a port past 3.
 */
int odi_switch_l2_ucast_static_encode(const uint8_t mac[6], uint32_t port, int valid,
				      uint32_t raw[ODI_SW_L2_WORDS]);

/* Write the static row through the hash (replacing a learned row with the
 * same key), or remove the row with mac's key, learned or static. The
 * return codes are odi_switch_l2_mcast_add()'s and _del()'s; the delete
 * writes the row's own words and looks the key up again, and answers -5
 * when the row is still there.
 */
int odi_switch_l2_ucast_static_add(const uint8_t mac[6], uint32_t port, uint32_t *index);
int odi_switch_l2_ucast_del(const uint8_t mac[6], int *found);

/* Every row in use deleted, the CAM included, by the row's own words.
 * Run once at bring-up, after the CAM is on, before anything writes a row:
 * the table survives a warm restart, so what an earlier image or the
 * stock firmware left (static pins, multicast groups, CAM residue) is
 * still there. *rep: rows found and how many static, passes made, rows
 * still in use afterwards. Returns 0, or the engine error that stopped it.
 * Caller holds odi_switch_lock.
 */
#define ODI_SW_L2_CLEAN_PASSES	2U

struct odi_sw_l2_clean_report {
	uint32_t found;		/* rows in use before the first deletion */
	uint32_t found_static;	/* of those, static ones */
	uint32_t passes;
	uint32_t left;		/* rows in use after the last pass */
};

int odi_switch_l2_clean(struct odi_sw_l2_clean_report *rep);

/* The set of pinned MACs, and the reconcile that keeps the table in step
 * with the MACs the stick uses (odi_switch_pin.c collects them). A sync
 * removes every pinned MAC no longer wanted and (re)writes every wanted
 * one, so a second sync with the same set rewrites the same rows and
 * changes nothing. Only MACs whose write succeeded stay in *pinned; a
 * failed one is tried again by the next sync. Caller holds
 * odi_switch_lock.
 */
#define ODI_SW_L2_PINS_MAX	4U

struct odi_sw_l2_pins {
	uint32_t n;
	uint8_t mac[ODI_SW_L2_PINS_MAX][6];
};

struct odi_sw_l2_pin_report {
	uint32_t added;				/* bit i: want->mac[i] newly pinned */
	uint32_t failed;			/* bit i: want->mac[i] could not be written */
	int rc[ODI_SW_L2_PINS_MAX];		/* per wanted MAC: 0 or the add error */
	uint32_t row[ODI_SW_L2_PINS_MAX];	/* per wanted MAC: the row on success */
	uint32_t removed_n;
	uint8_t removed[ODI_SW_L2_PINS_MAX][6];
	uint32_t stuck_n;			/* MACs no longer wanted whose row did not go */
	uint8_t stuck[ODI_SW_L2_PINS_MAX][6];
};

/* Adds mac to *set unless it is there already or refused; 1 when added. */
int odi_switch_l2_pins_add(struct odi_sw_l2_pins *set, const uint8_t mac[6]);

void odi_switch_l2_pins_sync(struct odi_sw_l2_pins *pinned, const struct odi_sw_l2_pins *want,
			     uint32_t port, struct odi_sw_l2_pin_report *rep);

#endif /* ODI_SWITCH_L2_H */
