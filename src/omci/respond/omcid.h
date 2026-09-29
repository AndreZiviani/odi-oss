/* What the modules of omcid share.
 *
 * omcid was one 3122-line main.c. The split is by what a piece TALKS TO,
 * not by what it is about: the MIB store answers questions about itself,
 * drv.c is the only thing that reaches the driver, apply*.c turn the MIB
 * into driver commands, and the three servers each own one transport.
 * Everything crossing those lines is declared here and nowhere else, and
 * `static` is dropped only where another module uses a symbol.
 */
#ifndef OMCID_H
#define OMCID_H

#include "sys.h"
#include "io.h"
#include "../nl.h"
#include "../omci.h"
#include "omci_mib.h"
#include "../omci_tmpfile.h"
#include "omci_autonomous.h"
#include "omci_drv.h"
#include "omci_gemflow.h"
#include "omci_caps.h"
#include "omci_bdgconn.h"
#include "omci_bridgeport.h"
#include "../omci_cli_proto.h"
#include "../omci_msgq.h"

#define NL_POLL_US     2000    /* see nl_open: this paces the CLI too */
#define CLI_RETRIES    100
#define CLI_RETRY_NS   (5 * 1000 * 1000)
#define DRV_OPT        0x310au
#define DRV_GET_SN     15
#define DRV_GET_DEVID  4
#define MIB_ROWS      192
#define MIB_ROW_MAX   64
#define MIB_TBL_ENTRY 16
#define MIB_TBL_NODES 64
#define MIB_TBL_NONE  0xff
#define OLT_ACC_CREATE 4
#define PRIQ_DEFAULT_WEIGHT 1
#define FLOW_MAX 64                      /* what this device reports, and the
					  * ceiling on what is honoured */
#define SERV_MAX 256                     /* the service-id limit, and the number of
					  * rows omcicli dump srvflow prints */
#define MBPCD_TP_PPTP_ETH_UNI   1
#define MBPCD_TP_IP_HOST        4
#define MBPCD_TP_GEM_IWTP       5
#define MBPCD_TP_VEIP          11
/* Class 130's UnmarkFrmOpt: 0 takes an unmarked frame's P-bit from its
 * DSCP, 1 uses the profile's default P-bit. Only 0 sends the map. */
#define MAP8021P_UNMARKED_DSCP_TO_PBIT 0
#define TCONT_MAX 32
#define V_RULE "=================================\n"
#define V_BANNER "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n"

/* ---------------------------------------------------------------- MIB store
 *
 * What the OLT creates and sets has to be remembered, or a get after a set
 * lies and the next MIB upload is empty again. The layout here is this
 * program's own -- attributes packed at their wire widths, in order -- because
 * nothing outside reads it; matching the vendor's row padding would buy
 * nothing.
 *
 * Rows are capped: the entities the line provisions are all 64 bytes or less
 * (ExtVlanTagOperCfgData is the largest at 64), and the ones that are not --
 * LargeString at 400 -- hold the ACS URL and credentials, which this has no
 * reason to keep. A truncated row is marked rather than silently short.
 */
#define MIB_ROWS      192
#define MIB_ROW_MAX   64

/* ------------------------------------------------- table attributes (171)
 *
 * One attribute in the model is not a value but a table: class 171's
 * ReceivedFrameVlanTaggingOperTable. The OLT sets it one 16-byte entry at a
 * time and the entries accumulate, so the flat row above -- which keeps
 * whatever was written last -- is exactly half of it.
 *
 * The vendor keeps both halves too. `mib_SetGetAttribute` in libomci_mib.so
 * memcpys the entry to its natural offset in the ME struct like any other
 * attribute, and `ExtVlanTagOperCfgDataDrvCfg` maintains a separate linked
 * list beside it. We do the same, for the same reason: the flat copy answers a
 * get, the list is what the dump prints.
 *
 * The three rules below are read out of that function, not out of G.988:
 *
 *   - an entry is KEYED by its filter half, bytes 0..7;
 *   - setting an entry whose key already exists REPLACES the rest of it;
 *   - an entry whose treatment half is all-ones, bytes 8..15 every bit set,
 *     DELETES the matching entry instead of storing it.
 *
 * The vendor pushes new entries at the head of its list, so its INDEX 0 is the
 * most recently set entry. This keeps that order, because the dump has to
 * match.
 *
 * Nothing here allocates: a fixed pool, shared by every row, with rows linked
 * through indices. ISP1's two instances hold three and two entries.
 */
#define MIB_TBL_ENTRY 16
#define MIB_TBL_NODES 64
#define MIB_TBL_NONE  0xff

struct mib_tblent {
	uint8_t next;                        /* MIB_TBL_NONE ends the list */
	uint8_t used;
	uint8_t data[MIB_TBL_ENTRY];
};

struct mib_row {
	uint16_t classId;
	uint16_t inst;
	/* Which attributes the OLT has actually written, same bit order as an
	 * OMCI attribute mask: bit (16 - k) for attribute k.
	 *
	 * A row exists as soon as ONE attribute is set, and the rest of it is
	 * zero -- which is not the same thing as "the value is zero". Without
	 * this mask a single set of ONU-G's AdminState (writable, oltAcc 3)
	 * created the row and every later get of SerialNum, VID or Version
	 * answered zeros: the identity the OLT ranged on, overwritten by an
	 * absence. apply_entity's own comment already states the rule -- an
	 * attribute that was never set is not a value, it is an absence -- and
	 * this is what makes the store obey it. */
	uint16_t written;
	uint8_t used;
	uint8_t truncated;
	uint8_t tbl_head;                    /* MIB_TBL_NONE when empty */
	uint8_t tbl_count;
	uint8_t data[MIB_ROW_MAX];
	/* The row as it was before the Set currently being applied.
	 *
	 * Several of the vendor's handlers act only when an attribute has
	 * CHANGED, comparing the new row against the one their first argument
	 * points at -- which is the pre-Set copy. Class 45's port-bridging and
	 * learning arms are gated that way, and so is class 298's whole rate
	 * limiter. Without this the guard cannot be expressed at all, and the
	 * choice was to send on every Set: more than the vendor sends, which is
	 * harmless but is not what the vendor does.
	 *
	 * `mib_write` snapshots the whole row before writing anything, so it is
	 * valid for exactly as long as the apply that follows. On a create the
	 * row is fresh and this is zeroes, which reads as "everything changed"
	 * -- also what the vendor sees. */
	uint8_t prev[MIB_ROW_MAX];
};

/* ---------------------------------------------------------------- GEM flows
 *
 * The driver's flow table is indexed by the flow id in word 0 of the
 * descriptor, and the id is the caller's to choose. The vendor keeps one table
 * per direction and picks the lowest free slot; so does this. Getting it wrong
 * is not a soft failure -- every flow sent with id 0 lands on top of the last
 * one, which is why a second downstream flow came back refused.
 */
struct flow_slot { uint16_t port; uint8_t used; };

/* createTcont hands back the driver's own T-CONT index, which is not the ME id
 * the OLT uses. A GEM flow names its T-CONT by that index -- sending the ME id
 * (0x8000 for the first one) is what made the upstream leg fail where the
 * downstream leg, which does not name a T-CONT, succeeded. The vendor keeps the
 * same mapping; this is why createTcont's caller is given the entity id it never
 * sends to the driver. */
#define TCONT_MAX 32
struct tcont_slot { uint16_t meId; uint16_t index; uint8_t used; };

/* Everything that crosses a module boundary. */
extern uint8_t serial[9];
/* Fill serial[] from the kernel, or the config store until the kernel has
 * one; cheap once the kernel has answered. 0 when serial[] is set. `log`
 * says where it came from on stdout -- only from the main loop, never while
 * a CLI reply owns the output sink. */
int serial_refresh(int log);
extern uint8_t devid[40];
extern uint8_t mib_data_sync;
const struct omci_class *find_class(uint16_t id);
uint16_t attr_width(const struct omci_class *c, unsigned k);
extern struct mib_tblent tblpool[MIB_TBL_NODES];
extern struct mib_row mib[MIB_ROWS];
int attr_offset(const struct omci_class *c, unsigned k);
uint16_t create_mask(const struct omci_class *c);
void tbl_free(struct mib_row *r);
int attr_is_big(const struct omci_class *c, unsigned k);
uint16_t tbl_serialise(const struct omci_class *c, uint16_t inst, unsigned k,
		       uint8_t *out, uint16_t max);

/* The Get / Get-Next staging area.
 *
 * A Get of a big attribute cannot answer inline, so it reports the byte length
 * and the OLT then walks the value 29 bytes at a time. Everything between the
 * two lives here.
 *
 * The vendor keeps two of these (gOmciMulGetData is 2 x 0x80204 and every path
 * indexes it by a context id checked against 2). One is enough for us: that
 * index comes off the message struct's first halfword, which is the OMCC
 * instance, and this device has one. */
#define GETNEXT_CHUNK  29                /* baseline: 32 - result - mask */
#define GETNEXT_MAX    (MIB_TBL_NODES * MIB_TBL_ENTRY)

struct getnext_ctx {
	uint16_t classId;
	uint16_t inst;
	uint16_t attr;                       /* 1-based, as the mask counts */
	uint16_t len;
	uint16_t chunks;
	uint8_t  primed;
	uint8_t  data[GETNEXT_MAX];
};

extern struct getnext_ctx getnext;
struct mib_row *mib_find(uint16_t cls, uint16_t inst);
struct mib_row *mib_add(uint16_t cls, uint16_t inst);
void mib_del(uint16_t cls, uint16_t inst);
int mib_count(void);
struct mib_row *mib_row_at(int i);
/* Every entity, autonomous and OLT-created, and one entity's effective
 * values: see mibstore.c. The CLI dumps go through these, never the rows
 * alone. */
struct mib_ent { uint16_t cls, inst; };
#define MIB_ENTS_MAX 512                 /* 301 autonomous + MIB_ROWS */
int mib_entities(uint16_t cls, int want_inst, uint16_t inst,
		 const struct mib_ent **out);
const struct mib_row *mib_view(const struct omci_class *c, uint16_t inst);
/* show.c: the dump behind `omcli mib` (vendor 0) and `omcicli mib get`
 * (vendor 1). Always ends with an "N rows" line; returns N. */
int mib_dump(uint16_t cls, int want_inst, uint16_t inst, int vendor);
extern int conn_dirty;
extern int qos_dirty;
void bdgconn_rebuild(void);
void us_qos_rebuild(void);
uint16_t mib_write(struct mib_row *r, const struct omci_class *c, uint16_t mask, const uint8_t *src, uint16_t srclen);
/* Whether attribute k differs from what the row held before this Set. */
int mib_changed(const struct mib_row *r, const struct omci_class *c, unsigned k);
uint16_t attr_value(const struct omci_class *c, uint16_t inst, unsigned k, uint8_t *out);
uint32_t row_u32(const struct mib_row *r, const struct omci_class *c, unsigned k);
extern int apply_hw;
int omci_drv_call(uint32_t cmd, void *buf, uint32_t len);
extern uint8_t pairbuf[8];
void *pair(uint32_t port, uint32_t value);
extern uint8_t caps[OMCI_CAPS_LEN];
extern int caps_ok;
extern int caps_from_arg;
int hex_nib(char c);
uint32_t caps_u32(unsigned off);
uint32_t caps_flows(void);
uint32_t priq_related_port(uint16_t meId);
extern struct flow_slot flow_us[FLOW_MAX], flow_ds[FLOW_MAX];
extern int bc_flow;
extern struct omci_bdgconn servtab[SERV_MAX];
int bdgconn_add(int ingress, uint16_t gemPort, uint32_t dir, const struct omci_vlan_oper *vr);
void gen_no_vlan_filter_rule(struct omci_vlan_oper *vr);
void gen_manual_vlan_rule(struct omci_vlan_oper *vr, int vid, int pri, int isMc);
extern struct tcont_slot tcont_map[TCONT_MAX];
void mib_reset_all(void);
extern uint16_t alarm_snapshot;

/* ---------------------------------------------------------------- snapshot
 *
 * omcid's own MIB state (the rows, the table-attribute pool, the flow and
 * T-CONT bookkeeping, the service table -- everything that maps a managed
 * entity to switch programming), persisted to tmpfs so a respawned process
 * can resume answering the OLT without re-registration. See snapshot.c and
 * docs/BOOT.md, "Resume without re-registration".
 */
#define SNAPSHOT_PATH          "/var/run/omcid-mib.snap"
#define SNAPSHOT_TMP_PATH      "/var/run/omcid-mib.snap.tmp"
#define RESUME_DECISION_PATH   "/var/run/omcid-resume-decision"
void snapshot_save(void);
void snapshot_invalidate(void);
/* onu_state: the driver's ONU state (5 == O5). Returns 1 and repopulates the
 * MIB and its bookkeeping when a valid, matching snapshot was loaded; 0
 * otherwise, leaving everything exactly as it was (empty, for a fresh
 * process) -- the caller then falls back to the ordinary re-registration
 * path. */
int snapshot_try_resume(uint32_t onu_state);
void snapshot_write_decision(int resumed);
/* `creating` separates a Create from a Set. Class 47 needs it: the stock stack
 * drives the MAC learning limit and the flooding mask from the create arm
 * unconditionally, and the traffic descriptors only from the set arm, gated
 * on the attribute being in the mask. */
void apply_entity(const struct omci_class *c, uint16_t inst, const struct mib_row *r, uint16_t mask, int creating);
void apply_delete(const struct omci_class *c, uint16_t inst);
int instance_is_autonomous(uint16_t cls, uint16_t inst);
/* drv.c: the switch port of a UNI entity from the capability slots, or -1. */
int uni_switch_port(uint16_t meId, uint8_t slot_type);
/* apply_qos.c */
int us_queue_referenced(uint16_t pqMe);
int flow_find(int ds, uint16_t port);
void bc_gem_update(void);
void bc_gem_withdraw(uint16_t going);
void apply_priq(uint16_t inst);
void apply_gem_ctp(const struct omci_class *c, const struct mib_row *r);
void qos_reset(void);
/* apply_uni.c */
uint32_t bridge_uni_port_mask(uint16_t bridge_id);
void dot1rl_apply(const struct omci_class *c, uint16_t inst,
		  const struct mib_row *r, int drop, int all);
void apply_pptp_uni(const struct omci_class *c, uint16_t inst,
		    const struct mib_row *r, uint16_t mask, int creating);
void apply_mbpcd(const struct omci_class *c, uint16_t inst,
		 const struct mib_row *r, uint16_t mask, int creating);
void delete_mbpcd(const struct omci_class *c, uint16_t inst);
void apply_ext_vlan_dscp(const struct omci_class *c, uint16_t inst,
			 const struct mib_row *r, uint16_t mask);
void apply_mapper_dscp(const struct omci_class *c, uint16_t inst,
		       const struct mib_row *r, uint16_t mask);
void apply_mbsp(const struct omci_class *c, uint16_t inst,
		const struct mib_row *r, uint16_t mask, int creating);
void delete_mbsp(uint16_t inst);
void uni_reset(void);
/* apply_bridge.c */
uint32_t veip_uni_port_mask(uint16_t veipId);
uint32_t all_eth_uni_mask(void);
void cli_row(const struct mib_row *r);
void v_banner(const struct omci_class *c);
int vendor_row_exists(const struct omci_class *c);
int vendor_row(const struct mib_row *r, const struct omci_class *c);
uint32_t cli_mib(void);
uint32_t cli_flows(void);
uint32_t cli_caps(void);
uint32_t cli_tcont(void);
uint32_t cli_state(void);
uint32_t cli_conn(void);
void qmap_dump(void);
uint32_t cli_bridge(void);
uint32_t cli_ident(void);
uint32_t cli_vlan(void);
uint32_t cli_cfgset(void);
uint32_t cli_help(void);
void onu_state_sample(void);
extern long cliq;
extern long reply_q;
extern struct omcli_req clireq;
extern struct omcli_rep clirep;
extern uint32_t cli_seq;
extern int cli_failed;
extern uint32_t cli_used;
void cli_sink(const char *s, int n);
const char *cli_arg(unsigned i);
int cli_num(unsigned i, uint32_t *out);
int cli_id(unsigned i, uint32_t *out);
int cli_poll(void);
extern long vq;
extern int vq_is_ours;
extern long vfile;
void vq_ensure(void);
int vq_poll(void);
extern uint8_t rxbuf[2048];
extern uint8_t frame[OMCI_FRAME_LEN];
extern volatile int nl_fd;
extern volatile uint32_t nl_tid;
void handle(int fd, uint32_t tid, const uint8_t *f);
/* OMCI frames handle() has taken, from the line or injected: the main loop
 * times the quiet second before its rebuilds on this, not on CLI traffic. */
extern unsigned long omci_frames_handled;

/* ------------------------------------------------------------ config store
 *
 * The identity the line authenticates with, out of the two XML files on the
 * jffs2 partition. See cfgstore.c: which key lives in which file comes from
 * xmlconfig's own descriptor table, and the LOID selection rule comes from
 * /etc/runomci.sh. */
struct onu_identity {
	char sn[32];
	char ploam[32];
	char loid[64];
	char loidPwd[64];
	int  snLen;                          /* -1 absent, 0 present and empty */
	int  ploamLen;
	uint8_t loaded;
};

/* The manual VLAN, which is where the service's tag actually comes from. */
struct onu_vlan_cfg {
	int mode;                            /* VLAN_MANU_MODE, 0 = off */
	int type;                            /* VLAN_CFG_TYPE */
	int vid;                             /* VLAN_MANU_TAG_VID, -1 absent */
	int pri;                             /* VLAN_MANU_TAG_PRI, 0 when absent */
	uint8_t manual;                      /* type 1, mode 1, vid and pri present */
	uint8_t loaded;
};

/* What the OLT is told about this ONU, from the store. See cfgstore.c. */
#define CFG_REPORT_SWITCH       "/var/config/omci-identity.on"
#define REPORT_DEFAULT_SW_VER   "0.0.0"
#define REPORT_SW_VER_LEN       14       /* class 7 attribute 1 */
#define REPORT_MODEL_LEN        20       /* class 257 attribute 1 */
#define REPORT_OMCC_VER_MAX     255      /* class 257 attribute 2, one byte */
#define REPORT_PRODUCT_CODE_MAX 65535    /* class 257 attribute 3, two bytes */
struct onu_report {
	char swVer[2][REPORT_SW_VER_LEN + 1];
	int  swVerLen[2];                    /* -1 absent, 0 empty */
	char model[REPORT_MODEL_LEN + 1];
	int  modelLen;
	int  omccVer;                        /* -1: absent, empty or not a number */
	int  productCode;                    /* likewise */
	uint8_t on;                          /* CFG_REPORT_SWITCH exists */
	uint8_t loaded;
};

extern struct onu_identity ident;
extern struct onu_vlan_cfg vlanCfg;
extern struct onu_report report;
void cfg_load_vlan(void);
void cfg_load_vlan_from(const char *cs);
int cfg_manual_vid(void);
void cfg_load_report(void);
void cfg_load_report_from(const char *cs, const char *hs, const char *sw);
const char *report_sw_ver(uint16_t inst);
void cfg_show_vlan(void);
int cfg_get(const char *path, const char *key, char *out, int max);
extern const char *const CFG_CS_PATH;
void cfg_load_identity(void);
void cfg_load_identity_from(const char *cs, const char *hs);
int cfg_set(const char *path, const char *dir, const char *key, const char *value);
void cfg_show_identity(void);

#endif
