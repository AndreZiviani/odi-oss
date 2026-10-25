/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_pon_isolation_test.c -- nothing from the fibre reaches the CPU, and
 * nothing the CPU sends by lookup reaches the fibre: the PON and CPU
 * ports' isolation masks (odi_switch_platform.c, odi_switch_port.c), the
 * NIC's drop of PON frames no RX hook took (odi_nic.c, odi_nic_hw.h), and
 * the port masks the NIC gives the IP stack's frames (odi_nic_hw.h).
 * docs/SWITCH.md, "PON port isolation".
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
#include "../kernel/extra/drivers/net/ethernet/odi/odi_igmp_tx.h"

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
	CHECK(word_at(0x27004U) == 0x003fd807U,
	      "port 2 (PON) forwards to ports 0, 1 and itself only; port 3 (CPU) to everything but port 2");
	CHECK(word_at(0x27008U) == 0x003fffffU && word_at(0x2700cU) == 0x003fffffU &&
	      word_at(0x27010U) == 0x003fffffU && word_at(0x27014U) == 0x000007ffU,
	      "the extension-port table is not touched");
	CHECK(writes_to(0x27004U) == 1, "one write of the PON port's word");
	CHECK(writes_to(0x27000U) == 0 && writes_to(0x27008U) == 0 && writes_to(0x2700cU) == 0 &&
	      writes_to(0x27010U) == 0 && writes_to(0x27014U) == 0,
	      "no write to any other isolation word");
	CHECK(iso_mask(PON) == 0x007U, "the PON port's decoded mask: ports 0-2, no CPU, no extension port");
	CHECK(iso_mask(CPU) == 0x7fbU, "the CPU port's decoded mask: all but the PON port, extension ports kept");
	CHECK(iso_mask(UNI) == 0x7ffU && iso_mask(PORT1) == 0x7ffU, "ports 0 and 1 decode all open");
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

	/* The host's traffic is untouched, toward the stick and the fibre. */
	CHECK(egress(UNI, 1U << CPU) == (1U << CPU), "UNI: the host still reaches the CPU");
	CHECK(egress(UNI, 0xfU) == ((1U << PORT1) | (1U << PON) | (1U << CPU)),
	      "UNI: host broadcast (ARP) still reaches the CPU and the fibre");
	CHECK(egress(UNI, 1U << PON) == (1U << PON), "UNI: the host's unicast to the fibre still gets there");

	/* What the CPU sends, should the switch forward it by lookup: the
	 * host only, never the fibre.
	 */
	CHECK(egress(CPU, 1U << UNI) == (1U << UNI), "CPU: replies to the host leave by the UNI");
	CHECK(egress(CPU, 0xfU) == ((1U << UNI) | (1U << PORT1)),
	      "CPU: the stick's broadcast (flood mask 0xf) reaches the UNIs, not the fibre");
	CHECK(egress(CPU, 0x7U) == ((1U << UNI) | (1U << PORT1)),
	      "CPU: the stick's unknown unicast (mask 0x7 after cmd 64) reaches the UNIs, not the fibre");
	CHECK(egress(CPU, 1U << PON) == 0,
	      "CPU: a lookup hit on the PON port (a MAC learned there) does not get there");
	/* The OMCI replies are a directed TX (PORT_SEL, the PON port mask):
	 * not a lookup, and not filtered by isolation per the vendor
	 * forwarding engine (it re-applies isolation to directed TX in
	 * software). That is a reading, not a measurement: the first trial
	 * shows it (docs/SWITCH.md, "PON port isolation"). Not modelled here.
	 */
}

/* The parameter: off restores the open mask, on narrows it again; each
 * apply is one write and a second apply changes nothing.
 */
static void test_parameter(void)
{
	boot();

	odi_sw_pon_isolate = false;
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x003fffffU, "off: the PON and CPU ports' masks back to all open");
	CHECK(word_at(0x27000U) == 0x003fffffU, "off: ports 0 and 1 untouched");

	odi_sw_pon_isolate = true;
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x003fd807U, "on again: narrowed");
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x003fd807U, "a second apply changes nothing");

	/* Only the PON bit of the CPU port's half moves; the rest is kept. */
	odi_mock.regs[odi_mock_slot(0x27004U)] = 0x00157fffU;	/* port 3: 0x2af, port 2: 0x7ff */
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x00155807U, "the CPU port's other bits are kept, bit 2 cleared");
	odi_sw_pon_isolate = false;
	odi_sw_pon_isolate_apply();
	CHECK(word_at(0x27004U) == 0x00157fffU, "off restores exactly the bits on cleared");
	odi_sw_pon_isolate = true;
}

/* The frames of the IP stack: the destination mask each device gives them,
 * as the TX descriptor carries it (opts3 bits 28:23), and the bits of opts1
 * (the TX descriptor layout: PORT_SEL is bit 18, the bit the OMCI replies
 * set to go onto a PON stream).
 */
static void test_nic_stack_tx(void)
{
	static const struct { unsigned int dev_port; unsigned int mask; const char *what; } dev[] = {
		{ 0xFFU, 0x3U, "eth0 (the catch-all): ports 0 and 1" },
		{ 0U, 0x1U, "eth0.2: the host port alone" },
		{ 1U, 0x2U, "eth0.3: port 1 alone" },
		{ 0xFEU, 0x0U, "nas0, pon0: nothing" },
		{ 2U, 0x0U, "a device claiming the PON port: nothing" },
		{ 3U, 0x0U, "a device claiming the CPU port: nothing" },
	};
	unsigned int i;

	CHECK(ODI_TXD_PORT_SEL == (1U << 18), "PORT_SEL is opts1 bit 18");
	CHECK(ODI_TXD_PORTS_SHIFT == 23U && ODI_TXD_PORTS_MASK == 0x3fU, "the port mask is opts3 bits 28:23");
	for (i = 0; i < sizeof(dev) / sizeof(dev[0]); i++) {
		unsigned int m = odi_nic_stack_tx_mask(dev[i].dev_port);
		struct odi_tx_desc t;

		CHECK(m == dev[i].mask, dev[i].what);
		CHECK(!(m & ((1U << PON) | (1U << CPU))), "never the PON port, never the CPU port");
		memset(&t, 0, sizeof(t));
		odi_nic_tx_pack(&t, m, 0, 0, 0);
		CHECK(((t.opts3 >> 23) & 0x3fU) == m, "the descriptor carries that mask");
		CHECK(!(t.opts1 & (1U << 18)), "and no PORT_SEL");
	}
	CHECK(odi_nic_stack_tx_mask(0xFFU) != 0 && odi_nic_stack_tx_mask(0U) != 0,
	      "a device that transmits never gets mask 0, the hardware lookup");
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

/* The IGMP way up (odi_igmp_tx.h, odi_igmp_rx.h): while igmpd takes the
 * host's reports and sends them up itself, the CPU gains a transmit toward
 * the fibre and nothing else. A frame from the PON is still not one the IGMP
 * hook takes, whatever the lease holds, so it still meets the NIC's drop; and
 * what igmpd sends is a directed transmit to the PON port alone, never learnt
 * from, never a lookup. */
static void test_igmp_way_up(void)
{
	/* An IGMPv2 report as it would arrive, untagged, and the descriptor of a
	 * frame from the PON port with the ACL reason of the snooping entry
	 * (128 + 8): the case where the hook would matter. */
	uint8_t f[60] = { 0x01, 0x00, 0x5e, 0x01, 0x01, 0x01, 0x02, 0, 0, 0, 0, 0x09, 0x08, 0x00,
			  0x46 };
	struct odi_rx_desc pon_igmp = { .opts1 = 0x36000040U, .opts2 = 0x80000000U,
					.opts3 = 0x20000000U | (136U << ODI_RXD_REASON_SHIFT) };
	struct odi_rx_desc host_igmp = { .opts1 = 0x36000040U, .opts2 = 0x80000000U,
					 .opts3 = 0x00200000U | (136U << ODI_RXD_REASON_SHIFT) };
	uint32_t o3;

	f[23] = 2;
	CHECK(odi_igmp_frame_wanted(f, sizeof f), "the frame is IGMP");
	CHECK(odi_nic_rx_from_pon(&pon_igmp) && odi_nic_rx_reason(&pon_igmp) == 136U,
	      "an IGMP frame from the PON with the snooping entry's reason");
	CHECK(!odi_igmp_rx_takes(1, odi_nic_rx_src_port(&pon_igmp), f, sizeof f),
	      "with the copy and the take held, the IGMP hook does not take it: the NIC drops it");
	CHECK(odi_igmp_rx_takes(1, odi_nic_rx_src_port(&host_igmp), f, sizeof f),
	      "the host's own report is taken");

	o3 = odi_igmp_tx_opts3(5);
	CHECK(((o3 >> ODI_TXD_PORTS_SHIFT) & ODI_TXD_PORTS_MASK) == (1U << PON),
	      "a report sent up names the PON port alone");
	CHECK(((o3 >> ODI_TXD_GEM_SHIFT) & ODI_TXD_GEM_MASK) == 5U, "on the flow it was given");
	CHECK((ODI_IGMP_TX_OPTS1 & ODI_TXD_PORT_SEL) && (ODI_IGMP_TX_OPTS1 & ODI_TXD_NO_LEARN),
	      "a directed transmit (PORT_SEL), never learnt from (the host's MAC stays on the host port)");
	CHECK(!odi_igmp_tx_frame_ok(f, 59) && odi_igmp_tx_frame_ok(f, sizeof f),
	      "and only an IGMP frame of a minimum frame's length or more goes");
	f[0] = 0xff;
	CHECK(!odi_igmp_tx_frame_ok(f, sizeof f), "never a broadcast");
}

int main(void)
{
	test_platform_words();
	test_forwarding_model();
	test_parameter();
	test_nic_descriptors();
	test_nic_stack_tx();
	test_igmp_way_up();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_pon_isolation_test: ok\n");
	return 0;
}
