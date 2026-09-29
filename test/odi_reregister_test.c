/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_reregister_test.c -- one boot of the ONU on ISP1, then two more
 * registrations in the same boot, and the upstream path of the OMCC checked
 * after every one of them.
 *
 * A registration here is what the driver sees when the ONU leaves O5 and
 * comes back: the gpondeact and gponact verbs (the omcid SIGHUP reload, a
 * respawned omcid and apply.sh all use them), the downstream PLOAM
 * messages an OLT sends to range and configure the ONU again (G.984.3
 * clause 10: Upstream_Overhead, Assign_ONU-ID, Ranging_Time,
 * Configure_Port-ID and Encrypted_Port-ID for the OMCC port, Assign_Alloc-ID
 * for the T-CONTs), and then the whole ISP1 provisioning again: the 91
 * driver commands of isp1_boot5_cmds.h, with the arguments omcid sends. The
 * third registration also carries two deallocations (Assign_Alloc-ID type
 * 255, G.984.3 9.2.3.10): of the ONU-ID itself, which is the default
 * Alloc-ID, and of one T-CONT Alloc-ID the OLT then assigns again.
 *
 * Upstream OMCI travels on the OMCC GEM port in the bursts of the default
 * Alloc-ID, the ONU-ID (G.984.3 clause 5.5.2). After each registration
 * this test requires the state that carries it to be the state the first
 * registration left:
 *
 *   - the Alloc-ID CAM, modelled from its request handshake: row 16 holds
 *     the ONU-ID, rows 0-4 the five T-CONT Alloc-IDs, nothing else;
 *   - the OMCC GEM port: US_GEM_PORT_MAP(64) and its stream-valid bit;
 *   - the scheduler words of the OMCC T-CONT, which the module-load replay
 *     (the real modload.bin) writes before any OMCI: PONQ_COUNT_MASK +206 = 1
 *     and bit 16 of +207;
 *   - every word of the PON queue block (PONQ_COUNT_MASK 0..300), the
 *     upstream and downstream GEM maps and PORT_QUEUE_MAP, word for word.
 *
 * The last check is the one that matters: a re-provisioning of the same MIB
 * must write the same hardware, so any word that drifts is reported by
 * name and number, not only the ones named above.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"

#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_gemflow.h"
#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_bdgconn.h"
#include "bdgconn_rules.h"
#include "isp1_boot5_cmds.h"

static int failures;

#define ONU_ID		26U	/* ISP1 assigns it; not an identity value */
#define OMCC_GEM_ROW	64U
#define OMCC_ALLOC_ROW	16U
#define PONQ_WORDS	301U

static const uint8_t dummy_sn[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
static const uint8_t no_pw[10];

/* ---- The Alloc-ID CAM, modelled from its handshake ----------------------
 *
 * odi_switch_gpon_alloc_write() (MODE 1) and the MODE 3 delete of
 * odi_gpon_hw.c both end with a DSF_ALLOC_CAM_CTL write that sets REQ; the
 * row is in bits 4:0, the mode in bits 9:8, and a write takes the Alloc-ID
 * from DSF_ALLOC_CAM_WDATA. The mock keeps only register values, so this
 * hook keeps the table.
 */
struct cam_row {
	int valid;
	uint32_t alloc_id;
};
static struct cam_row cam[32];

static void cam_hook(uint32_t off, uint32_t val)
{
	unsigned int row = val & 0x1fU, op = (val >> 8) & 0x3U;

	if (off != ODI_SW_DSF_ALLOC_CAM_CTL_OFF || !(val & (1U << 15)))
		return;
	if (op == 1U) {
		cam[row].valid = 1;
		cam[row].alloc_id = odi_mock.regs[odi_mock_slot(ODI_SW_DSF_ALLOC_CAM_WDATA_OFF)] & 0xfffU;
	} else if (op == 3U) {
		cam[row].valid = 0;
		cam[row].alloc_id = 0;
	}
}

/* ---- Downstream PLOAM, one interrupt pass per message -------------------- */

static void poke(uint32_t off, uint32_t val)
{
	odi_mock.regs[odi_mock_slot(off)] = val;
}

static void ploam(struct odi_gpon_fsm *fsm, uint8_t onu_id, uint8_t type,
		  const uint8_t content[10])
{
	struct odi_gpon_ploam msg;
	uint16_t words[6];
	unsigned int i, rep;

	msg.onu_id = onu_id;
	msg.type = type;
	memcpy(msg.content, content, sizeof(msg.content));
	odi_gpon_ploam_pack_words(&msg, words);

	/* Every downstream message is sent three times (G.984.3 9.1). */
	for (rep = 0; rep < 3U; rep++) {
		poke(ODI_GPON_PONMAC_IRQ_PENDING_OFF, ODI_GPON_PONMAC_IRQ_PENDING_DS_FRAMER);
		poke(ODI_GPON_DSF_IRQ_EVENT_OFF, ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX);
		poke(ODI_GPON_DSF_PLOAM_RX_CTL_OFF, 0U);
		for (i = 0; i < 6U; i++)
			poke(ODI_GPON_DSF_PLOAM_RX_WORD(i), words[i]);
		if (odi_gpon_isr_poll(fsm, NULL) != 1U) {
			fprintf(stderr, "FAIL: PLOAM type 0x%02x was not drained\n", type);
			exit(1);
		}
	}
}

static void alloc_msg(struct odi_gpon_fsm *fsm, uint16_t alloc_id, uint8_t type)
{
	uint8_t c[10] = { 0 };

	c[0] = (uint8_t)(alloc_id >> 4);
	c[1] = (uint8_t)((alloc_id & 0xfU) << 4);
	c[2] = type;
	ploam(fsm, ONU_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID, c);
}

/* The five Alloc-IDs ISP1 assigns, in its order (/proc/odi_gpon on ISP1). */
static const uint16_t isp1_alloc[5] = { 282, 794, 1050, 1306, 538 };

static void register_onu(struct odi_gpon_fsm *fsm, int cycle)
{
	static const uint8_t overhead[10] = { 0x20, 0x00, 0x00, 0xaa, 0xab, 0x59, 0x83, 0x20, 0x00, 0x00 };
	static const uint8_t ranging[10] = { 0x00, 0x00, 0x03, 0x77, 0x64 };
	static const uint8_t port[10] = { 0x01, 0x01, 0xa0 };	/* Port-ID 26, activate */
	static const uint8_t enc[10] = { 0x02, 0x01, 0xa0 };	/* Port-ID 26, valid, clear */
	uint8_t onu[10] = { 0 };
	unsigned int i;

	if (cycle > 0)
		odi_gpon_verb_deactivate(fsm);
	odi_gpon_verb_activate(fsm);

	ploam(fsm, ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_UPSTREAM_OVERHEAD, overhead);
	onu[0] = ONU_ID;
	memcpy(&onu[1], dummy_sn, sizeof(dummy_sn));
	ploam(fsm, ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_ASSIGN_ONU_ID, onu);
	ploam(fsm, ONU_ID, ODI_GPON_DS_RANGING_TIME, ranging);
	ploam(fsm, ONU_ID, ODI_GPON_DS_CONFIGURE_PORT_ID, port);
	ploam(fsm, ONU_ID, ODI_GPON_DS_ENCRYPTED_PORT_ID, enc);

	/* An OLT that takes the default Alloc-ID back: it belongs to the
	 * ONU-ID, so the row must stay. */
	if (cycle == 2)
		alloc_msg(fsm, ONU_ID, ODI_GPON_ALLOC_ID_TYPE_DEALLOCATE);
	for (i = 0; i < 5U; i++)
		alloc_msg(fsm, isp1_alloc[i], ODI_GPON_ALLOC_ID_TYPE_GEM);
	/* And a T-CONT Alloc-ID released and assigned again: it comes back
	 * in the row it had. */
	if (cycle == 2) {
		alloc_msg(fsm, isp1_alloc[1], ODI_GPON_ALLOC_ID_TYPE_DEALLOCATE);
		alloc_msg(fsm, isp1_alloc[1], ODI_GPON_ALLOC_ID_TYPE_GEM);
	}
}

/* ---- The OLT provisioning, as the driver commands omcid sends ------------ */

static void call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)odi_switch_cmd(cmd, buf, len);
}

/* ---- What the upstream path looks like ----------------------------------- */

struct snap {
	uint32_t ponq[PONQ_WORDS];
	uint32_t us_map[128];
	uint32_t ds_flow_type[128];
	uint32_t port_queue_map;
	struct cam_row cam[32];
};

static void take(struct snap *s)
{
	unsigned int i;

	for (i = 0; i < PONQ_WORDS; i++)
		s->ponq[i] = odi_reg_read(ODI_SW_PONQ_COUNT_MASK(i));
	for (i = 0; i < 128U; i++) {
		s->us_map[i] = odi_reg_read(ODI_SW_US_GEM_PORT_MAP(i));
		s->ds_flow_type[i] = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(i));
	}
	s->port_queue_map = odi_reg_read(ODI_SW_PORT_QUEUE_MAP_BASE);
	memcpy(s->cam, cam, sizeof(cam));
}

static void fail(int cycle, const char *what, unsigned int idx, uint32_t want, uint32_t got)
{
	fprintf(stderr, "FAIL: registration %d: %s[%u] is 0x%08x, the first registration left 0x%08x\n",
		cycle + 1, what, idx, got, want);
	failures++;
}

/* The OMCC path, by name: what upstream OMCI needs. */
static void check_omcc(int cycle, const struct odi_gpon_fsm *fsm, const struct snap *s)
{
	unsigned int row;

	if (fsm->state != ODI_GPON_STATE_O5 || fsm->onu_id != ONU_ID) {
		fprintf(stderr, "FAIL: registration %d: state %d onu_id %u, not O5 on %u\n",
			cycle + 1, (int)fsm->state, (unsigned int)fsm->onu_id, ONU_ID);
		failures++;
	}
	if (!s->cam[OMCC_ALLOC_ROW].valid || s->cam[OMCC_ALLOC_ROW].alloc_id != ONU_ID) {
		fprintf(stderr, "FAIL: registration %d: Alloc-ID CAM row 16 (the default Alloc-ID) is %s %u, not the ONU-ID %u\n",
			cycle + 1, s->cam[OMCC_ALLOC_ROW].valid ? "valid" : "empty",
			(unsigned int)s->cam[OMCC_ALLOC_ROW].alloc_id, ONU_ID);
		failures++;
	}
	for (row = 0; row < 5U; row++) {
		if (!s->cam[row].valid || s->cam[row].alloc_id != isp1_alloc[row]) {
			fprintf(stderr, "FAIL: registration %d: Alloc-ID CAM row %u is %u, not %u\n",
				cycle + 1, row, (unsigned int)s->cam[row].alloc_id,
				(unsigned int)isp1_alloc[row]);
			failures++;
		}
	}
	if (s->us_map[OMCC_GEM_ROW] != ONU_ID) {
		fprintf(stderr, "FAIL: registration %d: US_GEM_PORT_MAP(64), the OMCC port, is %u\n",
			cycle + 1, (unsigned int)s->us_map[OMCC_GEM_ROW]);
		failures++;
	}
	if (odi_reg_read(ODI_SW_PONQ_STREAM_VALID(2U)) != 1U) {
		fprintf(stderr, "FAIL: registration %d: the OMCC stream-valid bit is clear\n", cycle + 1);
		failures++;
	}
	if (s->ponq[206] != 1U) {
		fprintf(stderr, "FAIL: registration %d: PONQ_COUNT_MASK +206, the OMCC T-CONT scheduler word, is 0x%x, not 0x1\n",
			cycle + 1, s->ponq[206]);
		failures++;
	}
	if (!(s->ponq[207] & (1U << OMCC_ALLOC_ROW))) {
		fprintf(stderr, "FAIL: registration %d: PONQ_COUNT_MASK +207 is 0x%x: bit 16, the OMCC T-CONT, is clear\n",
			cycle + 1, s->ponq[207]);
		failures++;
	}
}

static void check_same(int cycle, const struct snap *first, const struct snap *now)
{
	unsigned int i;

	for (i = 0; i < PONQ_WORDS; i++)
		if (first->ponq[i] != now->ponq[i])
			fail(cycle, "PONQ_COUNT_MASK", i, first->ponq[i], now->ponq[i]);
	for (i = 0; i < 128U; i++) {
		if (first->us_map[i] != now->us_map[i])
			fail(cycle, "US_GEM_PORT_MAP", i, first->us_map[i], now->us_map[i]);
		if (first->ds_flow_type[i] != now->ds_flow_type[i])
			fail(cycle, "DSF_GEM_FLOW_TYPE", i, first->ds_flow_type[i], now->ds_flow_type[i]);
	}
	if (first->port_queue_map != now->port_queue_map)
		fail(cycle, "PORT_QUEUE_MAP", 0, first->port_queue_map, now->port_queue_map);
	for (i = 0; i < 32U; i++)
		if (first->cam[i].valid != now->cam[i].valid ||
		    first->cam[i].alloc_id != now->cam[i].alloc_id)
			fail(cycle, "Alloc-ID CAM row", i,
			     first->cam[i].valid ? first->cam[i].alloc_id : 0xffffffffU,
			     now->cam[i].valid ? now->cam[i].alloc_id : 0xffffffffU);
}

int main(void)
{
	static struct snap first, now;
	struct odi_replay_fw modload;
	struct odi_gpon_fsm fsm;
	int cycle;

	odi_mock_reset();
	odi_mock_write_hook = cam_hook;
	odi_switch_cmd_reset_state();
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(&fsm);
	odi_gpon_fsm_set_serial_number(&fsm, dummy_sn);
	odi_gpon_fsm_set_password(&fsm, no_pw);

	/* The module-load replay a boot runs before any OMCI: it sets up the
	 * scheduler of the OMCC T-CONT. */
	if (odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &modload)) {
		fprintf(stderr, "FAIL: modload.bin did not load\n");
		return 1;
	}
	odi_switch_init_modload(&modload.blob);
	odi_replay_fw_release(&modload);
	if (odi_reg_read(ODI_SW_PONQ_COUNT_MASK(206)) != 1U ||
	    odi_reg_read(ODI_SW_PONQ_COUNT_MASK(207)) != (1U << OMCC_ALLOC_ROW)) {
		fprintf(stderr, "FAIL: the module-load replay did not leave +206 = 1 and +207 = 0x10000; "
			"the OMCC checks below would test nothing\n");
		return 1;
	}

	for (cycle = 0; cycle < 3; cycle++) {
		register_onu(&fsm, cycle);
		isp1_boot5_replay(call);
		if (!odi_mock_locks_idle()) {
			fprintf(stderr, "FAIL: registration %d left a switch lock held\n", cycle + 1);
			failures++;
		}
		take(cycle == 0 ? &first : &now);
		check_omcc(cycle, &fsm, cycle == 0 ? &first : &now);
		if (cycle > 0)
			check_same(cycle, &first, &now);
	}

	if (failures) {
		fprintf(stderr, "odi_reregister_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_reregister_test: ok (3 registrations in one boot; the OMCC path and the "
	       "whole upstream queue state are the same after each)\n");
	return 0;
}
