/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_pon_isolation_test.c -- nothing from the fibre reaches the CPU:
 * the PON port's isolation mask (odi_switch_platform.c, odi_switch_port.c)
 * and the NIC's drop of PON frames no RX hook took (odi_nic.c,
 * odi_nic_hw.h). docs/SWITCH.md, "PON port isolation".
 *
 * Not circular: the inputs are what the hardware gave, not what our code
 * produced.
 *   - The switch side starts from the isolation words a running stick
 *     reads (0x003fffff in five words, 0x000007ff in the sixth: two 11-bit
 *     items a word, the layout of the stock register table: 0x027000
 *     width 11 ports 0..3, 0x027008 width 11 items 0..6, next register
 *     0x027018). The expected words are written out here as numbers and
 *     decoded by this file's own decoder, not by odi_switch_hw.h's
 *     accessors. A small forwarding model then asks, for each kind of frame
 *     the brief names, which ports it may reach.
 *   - The NIC side takes RX descriptor words logged on a stick: an OMCI
 *     frame that reached omcid, and host frames that reached br0.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_nic_hw.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* Board ports. */
#define UNI	0U
#define PORT1	1U
#define PON	2U
#define CPU	3U

/* The isolation mask of an ingress port, decoded here: two 11-bit items a
 * word, port n in the word at 0x027000 + 4 * (n / 2), at bit 11 * (n % 2).
 */
static uint32_t iso_mask(uint32_t port)
{
	uint32_t word = odi_mock.regs[odi_mock_slot(0x27000U + 4U * (port / 2U))];

	return (word >> (11U * (port % 2U))) & 0x7ffU;
}

static uint32_t word_at(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

/* The ports a frame may leave by: the candidates the lookup gives (the L2
 * hit, or a flood mask), less the ingress port, filtered by the ingress
 * port's isolation mask (bits 3:0, the switch ports). A trap to the CPU
 * is a candidate set of the CPU port alone.
 */
static uint32_t egress(uint32_t ingress, uint32_t candidates)
{
	return candidates & ~(1U << ingress) & iso_mask(ingress) & 0xfU;
}

static int writes_to(uint32_t off)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == off)
			n++;
	return (int)n;
}

static void boot(void)
{
	odi_mock_reset();
	odi_mock_port_isolation_reset_state();
	odi_sw_pon_isolate = true;
	odi_switch_init_platform();
}

/* The platform settings narrow the PON port's mask, and nothing else. */
static void test_platform_words(void)
{
	boot();

	CHECK(word_at(0x27000U) == 0x003fffffU, "ports 0 and 1 keep the open mask");
	CHECK(word_at(0x27004U) == 0x003ff807U,
	      "port 2 (PON) forwards to ports 0, 1 and itself only; port 3 (CPU) keeps the open mask");
	CHECK(word_at(0x27008U) == 0x003fffffU && word_at(0x2700cU) == 0x003fffffU &&
	      word_at(0x27010U) == 0x003fffffU && word_at(0x27014U) == 0x000007ffU,
	      "the extension-port table is not touched");
	CHECK(writes_to(0x27004U) == 1, "one write of the PON port's word");
	CHECK(writes_to(0x27000U) == 0 && writes_to(0x27008U) == 0 && writes_to(0x2700cU) == 0 &&
	      writes_to(0x27010U) == 0 && writes_to(0x27014U) == 0,
	      "no write to any other isolation word");
	CHECK(iso_mask(PON) == 0x007U, "the PON port's decoded mask: ports 0-2, no CPU, no extension port");
}

/* Every kind of frame the fibre can send, against the stick's own paths. */
static void test_forwarding_model(void)
{
	boot();

	/* From the fibre. */
	CHECK(egress(PON, 1U << CPU) == 0,
	      "PON: unicast to the stick MAC (learned on the CPU port) reaches nothing");
	CHECK(egress(PON, 0xfU) == ((1U << UNI) | (1U << PORT1)),
	      "PON: broadcast (flood mask 0xf) reaches the UNIs, not the CPU");
	CHECK(egress(PON, 0xfU) == ((1U << UNI) | (1U << PORT1)),
	      "PON: unknown unicast with the CPU in the flood mask reaches the UNIs only");
	CHECK(egress(PON, 0x7U) == ((1U << UNI) | (1U << PORT1)),
	      "PON: unknown multicast (mask 0x7) reaches the UNIs");
	CHECK(egress(PON, 1U << CPU) == 0, "PON: a frame trapped to the CPU reaches nothing");
	CHECK(egress(PON, 1U << UNI) == (1U << UNI), "PON: a frame for a host behind the UNI still gets there");

	/* The stick's management and the OMCI replies are untouched. */
	CHECK(egress(UNI, 1U << CPU) == (1U << CPU), "UNI: the host still reaches the CPU");
	CHECK(egress(UNI, 0xfU) == ((1U << PORT1) | (1U << PON) | (1U << CPU)),
	      "UNI: host broadcast (ARP) still reaches the CPU");
	CHECK(egress(CPU, 1U << UNI) == (1U << UNI), "CPU: replies to the host leave by the UNI");
	CHECK(egress(CPU, 1U << PON) == (1U << PON), "CPU: OMCI replies still reach the PON port");
}

/* The parameter: off restores the open mask, on narrows it again; each
 * apply is one write and a second apply changes nothing.
 */
static void test_parameter(void)
{
	boot();

	odi_sw_pon_isolate = false;
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x003fffffU, "off: the PON port's mask back to all open");
	CHECK(word_at(0x27000U) == 0x003fffffU, "off: ports 0 and 1 untouched");

	odi_sw_pon_isolate = true;
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x003ff807U, "on again: narrowed");
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x003ff807U, "a second apply changes nothing");

	/* Port 3's half of the word survives a different value there. */
	odi_mock.regs[odi_mock_slot(0x27004U)] = 0x00155fffU;	/* port 3: 0x2ab, port 2: 0x7ff */
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x00155807U, "the CPU port's half of the word is kept");
}

/* RX descriptor words logged on a stick (odi_nic.debug), with the meaning
 * the frames had: an OMCI frame (it reached omcid) and two host frames
 * (they reached br0).
 */
static void test_nic_descriptors(void)
{
	struct odi_rx_desc omci = { .opts1 = 0x3600c038U, .opts2 = 0x84000000U, .opts3 = 0x203edc00U };
	struct odi_rx_desc host1 = { .opts1 = 0x36000048U, .opts2 = 0x80000000U, .opts3 = 0x00200000U };
	struct odi_rx_desc host2 = { .opts1 = 0x36008040U, .opts2 = 0x80000000U, .opts3 = 0x0fe00000U };
	/* A frame from the PON port with no trap reason: what a data frame
	 * the switch let through would look like (no stick has shown one).
	 */
	struct odi_rx_desc pon_data = { .opts1 = 0x36000040U, .opts2 = 0x80000000U, .opts3 = 0x20000000U };

	CHECK(ODI_NIC_PON_PORT == PON, "the NIC's PON port is the board's");
	CHECK(odi_nic_rx_from_pon(&omci), "the OMCI frame came from the PON port");
	CHECK(odi_nic_rx_reason(&omci) == 246U,
	      "it carries reason 246, which odi_omci's hook takes before the PON drop");
	CHECK(!odi_nic_rx_from_pon(&host1) && odi_nic_rx_src_port(&host1) == UNI,
	      "a host frame is from the UNI, not dropped");
	CHECK(!odi_nic_rx_from_pon(&host2) && odi_nic_rx_src_port(&host2) == UNI,
	      "a host broadcast is from the UNI, not dropped");
	CHECK(odi_nic_rx_from_pon(&pon_data) && odi_nic_rx_reason(&pon_data) != 246U,
	      "a PON data frame is from the PON with no OMCI reason: no hook takes it, the poll drops it");
}

int main(void)
{
	test_platform_words();
	test_forwarding_model();
	test_parameter();
	test_nic_descriptors();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_pon_isolation_test: ok\n");
	return 0;
}
