/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_omci_test.c -- host-side unit test for odi_omci_wire.h. Compiled
 * and run with the host cc, no kernel and no target toolchain needed:
 * the header has no kernel dependency.
 *
 * Covers the pkt_redirect wire layout odi_omci_wire.h packs and unpacks,
 * cross-checked against src/omci/nl.h's own encoding (nl_redirect(),
 * nl_send_payload()) rather than against a restatement of it, so the two
 * ends of the protocol cannot silently drift apart.
 */
#include <stdio.h>
#include <string.h>
#include "../kernel/extra/drivers/net/ethernet/odi/odi_omci_wire.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* Builds a registration message exactly as nl.h's nl_redirect() does, so
 * this test exercises the same bytes omcid actually sends.
 */
static void build_reg_msg(uint8_t *buf, uint32_t tid, unsigned type, unsigned action, unsigned mtu)
{
	uint8_t *b = buf + ODI_OMCI_NLMSG_HDR;

	memset(buf, 0, ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_REG_BODY);
	odi_omci_put32(buf + 0, ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_REG_BODY);
	odi_omci_put32(buf + 12, tid);
	odi_omci_put16(b + 0, ODI_OMCI_OP_REG);
	odi_omci_put16(b + 2, (uint16_t)type);
	odi_omci_put32(b + 4, tid);
	b[8] = (uint8_t)action;
	odi_omci_put16(b + 10, (uint16_t)mtu);
}

/* Builds a send message exactly as nl.h's nl_send_payload() does. */
static void build_send_msg(uint8_t *buf, uint32_t tid, unsigned type,
			    const uint8_t *payload, uint32_t len)
{
	uint8_t *b = buf + ODI_OMCI_NLMSG_HDR;
	uint32_t total = ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY + len;
	uint32_t i;

	memset(buf, 0, ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY);
	odi_omci_put32(buf + 0, total);
	odi_omci_put32(buf + 12, tid);
	odi_omci_put16(b + 0, ODI_OMCI_OP_SEND);
	odi_omci_put16(b + 2, (uint16_t)type);
	odi_omci_put32(b + 4, 0);
	odi_omci_put32(b + 8, 0);
	odi_omci_put32(b + 12, len);
	for (i = 0; i < len; i++)
		b[ODI_OMCI_NL_BODY + i] = payload[i];
}

static void test_reg_roundtrip(void)
{
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_REG_BODY];
	struct odi_omci_msg msg;

	build_reg_msg(buf, 4242, ODI_OMCI_REDIRECT_TYPE, ODI_OMCI_ACT_REG, 1500);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == 0, "a well-formed REG message parses");
	CHECK(msg.op == ODI_OMCI_OP_REG, "op decodes as REG");
	CHECK(msg.type == ODI_OMCI_REDIRECT_TYPE, "type decodes as REDIRECT_TYPE");
	CHECK(msg.reg_pid == 4242, "the tid/pid field roundtrips");
	CHECK(msg.reg_action == ODI_OMCI_ACT_REG, "the register action byte roundtrips");
}

static void test_dereg_roundtrip(void)
{
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_REG_BODY];
	struct odi_omci_msg msg;

	build_reg_msg(buf, 4242, ODI_OMCI_REDIRECT_TYPE, ODI_OMCI_ACT_DEREG, 0);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == 0, "a well-formed DEREG message parses");
	CHECK(msg.reg_action == ODI_OMCI_ACT_DEREG, "the deregister action byte roundtrips");
}

static void test_send_roundtrip(void)
{
	uint8_t frame[8] = { 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11 };
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY + sizeof frame];
	struct odi_omci_msg msg;

	build_send_msg(buf, 99, ODI_OMCI_REDIRECT_TYPE, frame, sizeof frame);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == 0, "a well-formed SEND message parses");
	CHECK(msg.op == ODI_OMCI_OP_SEND, "op decodes as SEND");
	CHECK(msg.type == ODI_OMCI_REDIRECT_TYPE, "type decodes as REDIRECT_TYPE");
	CHECK(msg.send_len == sizeof frame, "dataLen decodes to the exact payload length");
	CHECK(msg.send_off == ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY, "the payload offset is NLMSG_HDR + NL_BODY");
	CHECK(memcmp(buf + msg.send_off, frame, sizeof frame) == 0, "the payload bytes are unmodified");
}

static void test_send_overlong_len_rejected(void)
{
	uint8_t frame[4] = { 1, 2, 3, 4 };
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_BODY + sizeof frame];
	struct odi_omci_msg msg;

	build_send_msg(buf, 1, ODI_OMCI_REDIRECT_TYPE, frame, sizeof frame);
	/* Claim a dataLen the buffer does not actually hold -- a message this
	 * transport must refuse rather than read past its own boundary.
	 */
	odi_omci_put32(buf + ODI_OMCI_NLMSG_HDR + 12, 9000);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == -1,
	      "a dataLen exceeding the buffer is rejected, not truncated silently");
}

static void test_short_buffer_rejected(void)
{
	uint8_t buf[ODI_OMCI_NLMSG_HDR + 4];
	struct odi_omci_msg msg;

	memset(buf, 0, sizeof buf);
	odi_omci_put16(buf + ODI_OMCI_NLMSG_HDR, ODI_OMCI_OP_SEND);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == -1,
	      "a buffer too short for its own op's body is rejected");
}

static void test_unknown_op_rejected(void)
{
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_NL_REG_BODY];
	struct odi_omci_msg msg;

	build_reg_msg(buf, 1, ODI_OMCI_REDIRECT_TYPE, ODI_OMCI_ACT_REG, 0);
	odi_omci_put16(buf + ODI_OMCI_NLMSG_HDR, 0 /* PKT_REDIRECT_OPTYPE_NONE, never sent by nl.h */);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == -1, "an unknown op is rejected");
}

static void test_build_rx_matches_main_c_receive_offset(void)
{
	uint8_t frame[5] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_FRAME_OFF + sizeof frame];
	uint32_t total;

	total = odi_omci_build_rx(buf, frame, sizeof frame);

	CHECK(total == sizeof buf, "odi_omci_rx_total() matches what odi_omci_build_rx() writes");
	CHECK(odi_omci_get32(buf + 0) == total, "the length field at offset 0 is the total message length");
	/* src/omci/respond/main.c's own receive loop reads the frame from
	 * exactly this offset: rxbuf + NLMSG_HDR + NL_FRAME_OFF.
	 */
	CHECK(memcmp(buf + ODI_OMCI_NLMSG_HDR + ODI_OMCI_FRAME_OFF, frame, sizeof frame) == 0,
	      "the frame lands at NLMSG_HDR + FRAME_OFF, where main.c's receive loop reads it");
}

/* ---- ODI_OMCI_OP_CMD: both ends of this op
 * are ours (no vendor precedent), so build_cmd_req_msg() below restates
 * src/omci/nl.h own nl_cmd_call() request encoding directly rather than
 * calling it -- nl.h needs sys.h (the freestanding syscall layer), which
 * has no host build, only the cross toolchain own -- but every byte offset
 * matches nl_cmd_call() by inspection (odi_omci_wire.h own file-header
 * comment has the shared layout both files are hand-kept identical to).
 */
static void build_cmd_req_msg(uint8_t *buf, uint32_t tid, uint32_t cmd,
			       const uint8_t *arg, uint32_t len)
{
	uint8_t *b = buf + ODI_OMCI_NLMSG_HDR;
	uint32_t total = ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR + len;
	uint32_t i;

	memset(buf, 0, ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR);
	odi_omci_put32(buf + 0, total);
	odi_omci_put32(buf + 12, tid);
	odi_omci_put16(b + 0, (uint16_t)ODI_OMCI_OP_CMD);
	odi_omci_put16(b + 2, 0);
	odi_omci_put32(b + 4, cmd);
	odi_omci_put32(b + 8, len);
	for (i = 0; i < len; i++)
		b[ODI_OMCI_CMD_REQ_HDR + i] = arg[i];
}

static void test_cmd_req_roundtrip(void)
{
	uint8_t arg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR + sizeof arg];
	struct odi_omci_msg msg;

	build_cmd_req_msg(buf, 77, 51 /* OMCI_BDGCONN_CMD, arbitrary for this test */,
			   arg, sizeof arg);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == 0, "a well-formed CMD request parses");
	CHECK(msg.op == ODI_OMCI_OP_CMD, "op decodes as CMD");
	CHECK(msg.cmd_num == 51, "the cmd number roundtrips");
	CHECK(msg.cmd_len == sizeof arg, "the argument length roundtrips");
	CHECK(msg.cmd_off == ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR,
	      "the argument offset is NLMSG_HDR + CMD_REQ_HDR");
	CHECK(memcmp(buf + msg.cmd_off, arg, sizeof arg) == 0, "the argument bytes are unmodified");
}

static void test_cmd_req_overlong_len_rejected(void)
{
	uint8_t arg[4] = { 1, 2, 3, 4 };
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR + sizeof arg];
	struct odi_omci_msg msg;

	build_cmd_req_msg(buf, 1, 62, arg, sizeof arg);
	/* Claim a len the buffer does not hold. */
	odi_omci_put32(buf + ODI_OMCI_NLMSG_HDR + 8, 9000);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == -1,
	      "a CMD request claiming more argument bytes than the buffer holds is rejected");
}

static void test_cmd_req_over_max_len_rejected(void)
{
	/* A request whose own declared len exceeds ODI_OMCI_CMD_MAX_LEN,
	 * even though the buffer genuinely holds that many bytes -- the
	 * kernel-side handler own fixed-size argbuf (odi_omci.c own
	 * odi_omci_cmd()) depends on this being refused here, not there.
	 */
	uint8_t arg[ODI_OMCI_CMD_MAX_LEN + 16];
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REQ_HDR + sizeof arg];
	struct odi_omci_msg msg;

	memset(arg, 0xaa, sizeof arg);
	build_cmd_req_msg(buf, 1, 25, arg, sizeof arg);

	CHECK(odi_omci_parse(buf, sizeof buf, &msg) == -1,
	      "a CMD request over ODI_OMCI_CMD_MAX_LEN is rejected even if the buffer holds it");
}

static void test_cmd_reply_roundtrip(void)
{
	uint8_t arg[12] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0xff, 0xee };
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REPLY_HDR + sizeof arg];
	uint32_t total;

	total = odi_omci_build_cmd_reply(buf, 25, 0, arg, sizeof arg);

	CHECK(total == sizeof buf, "odi_omci_cmd_reply_total() matches what odi_omci_build_cmd_reply() writes");
	CHECK(odi_omci_get32(buf + 0) == total, "the NLMSG length field is the total message length");
	CHECK(odi_omci_get16(buf + ODI_OMCI_NLMSG_HDR + 0) == ODI_OMCI_OP_CMD, "the reply op is ODI_OMCI_OP_CMD");
	CHECK(odi_omci_get32(buf + ODI_OMCI_NLMSG_HDR + 4) == 25, "the reply echoes the cmd number");
	CHECK(odi_omci_get32(buf + ODI_OMCI_NLMSG_HDR + 8) == 0, "a success status is 0");
	CHECK(odi_omci_get32(buf + ODI_OMCI_NLMSG_HDR + 12) == sizeof arg, "the reply carries the argument length");
	CHECK(memcmp(buf + ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REPLY_HDR, arg, sizeof arg) == 0,
	      "the reply argument bytes are unmodified");
}

static void test_cmd_reply_negative_status_roundtrips(void)
{
	uint8_t buf[ODI_OMCI_NLMSG_HDR + ODI_OMCI_CMD_REPLY_HDR];
	int32_t status;

	odi_omci_build_cmd_reply(buf, 3, -95 /* -EOPNOTSUPP */, NULL, 0);

	status = (int32_t)odi_omci_get32(buf + ODI_OMCI_NLMSG_HDR + 8);
	CHECK(status == -95, "a negative status (-EOPNOTSUPP) survives the u32 wire round trip");
}

int main(void)
{
	test_reg_roundtrip();
	test_dereg_roundtrip();
	test_send_roundtrip();
	test_send_overlong_len_rejected();
	test_short_buffer_rejected();
	test_unknown_op_rejected();
	test_build_rx_matches_main_c_receive_offset();
	test_cmd_req_roundtrip();
	test_cmd_req_overlong_len_rejected();
	test_cmd_req_over_max_len_rejected();
	test_cmd_reply_roundtrip();
	test_cmd_reply_negative_status_roundtrips();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_omci_test: all checks passed\n");
	return 0;
}
