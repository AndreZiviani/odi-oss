/* Restated copy of kernel/extra/drivers/net/ethernet/odi/odi_reg.h (and,
 * for the L2 table, odi_switch_l2.h) -- the /dev/odi_sw ioctl ABI. The two trees build separately (this one
 * freestanding, no libc, under src/diag's own cross Makefile; the kernel
 * one under kbuild), so this is a second reading of the same numbers
 * rather than a shared #include -- keep the two identical. See that
 * header for what each ioctl does and why the request numbers are
 * hand-composed instead of built from <linux/ioctl.h> macros (MIPS o32's
 * direction bits differ from the asm-generic ones the macros assume).
 */
#ifndef ODI_SW_IOCTL_H
#define ODI_SW_IOCTL_H

#include <stdint.h>

struct odi_sw_reg {
	uint32_t addr;
	uint32_t value;
};

struct odi_sw_mib {
	uint32_t port;
	uint32_t counter;
	uint64_t value;
};

/* type is one of hw.h's enum ddm_sel, 0-6. raw is the 24-byte DDMI
 * block. */
struct odi_sw_ddm {
	uint32_t type;
	uint8_t raw[24];
};

/* The L2 lookup table, odi_switch_l2.h in the kernel tree: one row, its
 * three raw words (hardware bit b in raw[b / 32]) and the decode. */
struct odi_sw_l2_row {
	uint32_t index;
	uint32_t raw[3];
	uint8_t  mac[6];
	uint16_t key;           /* unicast: learned VID; multicast: VID (IVL) or FID (SVL) */
	uint8_t  type;          /* ODI_SW_L2_UCAST / _MCAST / _IPMC */
	uint8_t  port;
	uint8_t  ext_port;
	uint8_t  age;
	uint32_t flags;         /* ODI_SW_L2_F_* */
	uint32_t fid;
	uint32_t ports;
	uint32_t ext_ports;
	uint32_t group;
};

#define ODI_SW_L2_UCAST 0
#define ODI_SW_L2_MCAST 1
#define ODI_SW_L2_IPMC  2

#define ODI_SW_L2_F_VALID     (1u << 0)   /* the engine HIT on the read, not a row bit */
#define ODI_SW_L2_F_STATIC    (1u << 1)
#define ODI_SW_L2_F_IVL       (1u << 2)
#define ODI_SW_L2_F_CTAG      (1u << 3)
#define ODI_SW_L2_F_AUTH      (1u << 4)
#define ODI_SW_L2_F_SA_BLOCK  (1u << 5)
#define ODI_SW_L2_F_DA_BLOCK  (1u << 6)
#define ODI_SW_L2_F_ARP       (1u << 7)

struct odi_sw_l2_mcast_req {
	uint8_t  mac[6];
	uint16_t key;
	uint32_t ivl;
	uint32_t ports;
	uint32_t ext_ports;
};

struct odi_sw_l2_mcast {
	struct odi_sw_l2_mcast_req req;
	uint32_t index;         /* out: the row an add landed on */
	uint32_t found;         /* out: a delete matched a row */
};

struct odi_sw_l2_mode {
	uint32_t ipmc_on_group;   /* 0: IPv4 multicast switched on MAC + VID/FID */
	uint32_t rows;          /* 1024, or 1088 with the CAM rows on */
};

/* IGMP snooping lease: flags below. Held by the open file that sets them;
 * its close puts the switch back and deletes the rows it added. 0 releases. */
struct odi_sw_mc_snoop {
	uint32_t flags;
	uint32_t reserved;
};
#define ODI_SW_MC_SNOOP_COPY      1u   /* copy the host port's IGMP to the CPU */
#define ODI_SW_MC_SNOOP_PON_DROP  2u   /* drop unregistered IPv4 multicast from the PON */

/* ACL probes, odi_switch_acl_probe.h in the kernel tree: hand-installed
 * rules that count IPv4 frames entering the UNI or the PON port, or copy or
 * take the IGMP entering the UNI to the CPU. SET and DEL belong to the open
 * file that set first, until it closes (which removes them all); GET is
 * open to anyone. */
struct odi_sw_acl_probe {
	uint32_t op;            /* ODI_SW_PROBE_SET / _GET / _DEL */
	uint32_t slot;          /* 0..7, or ODI_SW_PROBE_ALL for a DEL */
	uint32_t port;          /* 0 the UNI, 2 the PON */
	uint32_t action;        /* ODI_SW_PROBE_COUNT / _MIRROR / _TRAP */
	uint32_t bytes;         /* 1: count bytes, 0: frames */
	uint32_t proto;         /* IPv4 protocol, or ODI_SW_PROBE_ANY */
	uint32_t dst;           /* IPv4 destination */
	uint32_t dst_mask;      /* the bits of dst compared */
	uint32_t in_use;        /* GET: the slot holds a rule */
	uint32_t entry;         /* its ACL entry */
	uint32_t counter;       /* its log counter */
	uint32_t base;          /* the counter when the rule went in */
	uint32_t now;           /* GET: the counter now */
	uint32_t cpu_hits;      /* GET: frames the CPU received from this rule */
	uint32_t cpu_ports;     /* GET: the ports those came in by */
	uint32_t uni_igmp_other; /* GET: IGMP from the UNI at the CPU, another reason */
	uint32_t other_reason;  /* GET: the last such reason */
	uint32_t cpu_bytes;     /* GET: the length of the cpu_hits frames */
};
#define ODI_SW_PROBE_SET     1u
#define ODI_SW_PROBE_GET     2u
#define ODI_SW_PROBE_DEL     3u
#define ODI_SW_PROBE_COUNT   1u
#define ODI_SW_PROBE_MIRROR  2u
#define ODI_SW_PROBE_TRAP    3u
#define ODI_SW_PROBE_ANY     0x100u
#define ODI_SW_PROBE_ALL     0xffffffffu
#define ODI_SW_PROBE_SLOTS   8u

#define ODI_SW_IOC_REG_GET	0xC0085301U
#define ODI_SW_IOC_REG_SET	0x80085302U
#define ODI_SW_IOC_MIB_GET	0xC0105304U
#define ODI_SW_IOC_DDM_GET	0xC01C5305U
#define ODI_SW_IOC_L2_GET	0xC0305306U
#define ODI_SW_IOC_L2_NEXT	0xC0305307U
#define ODI_SW_IOC_L2_MC_ADD	0xC01C5308U
#define ODI_SW_IOC_L2_MC_DEL	0xC01C5309U
#define ODI_SW_IOC_L2_MODE	0x4008530AU
#define ODI_SW_IOC_MC_SNOOP	0x8008530BU
#define ODI_SW_IOC_ACL_PROBE	0xC048530CU

#endif
