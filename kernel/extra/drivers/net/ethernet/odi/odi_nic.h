/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_nic.h -- the odi_nic.c <-> odi_omci.c interface: the RX/TX descriptor
 * word copies and the rxhook registration API. Both files used to carry
 * their own copy of every declaration below (odi_nic.c's own comment said
 * so outright: "it has no shared header"); this header is that header.
 *
 * struct odi_tx_words/odi_rx_words are copies of the TX/RX descriptor
 * words themselves, not separate wire structs. odi_nic_tx_words() takes a
 * raw frame, its length and the words the caller filled. An rxhook
 * (odi_rxhook_fn) returns ODI_RXHOOK_STOP, CONTINUE or STOP_NOFREE.
 *
 * Built-in only (CONFIG_ODI_NIC/CONFIG_ODI_OMCI are both bool): no
 * EXPORT_SYMBOL on the three functions below, a plain declaration is
 * enough for built-in-to-built-in linkage.
 */
#ifndef ODI_NIC_H
#define ODI_NIC_H

#include <linux/types.h>

struct sk_buff;

/* The RX descriptor words: a hook reads reason, source port, stream id
 * and VLAN fields out of them (the OMCI hook keys on reason 246 on this
 * chip). A hook gets a copy of the four words.
 */
struct odi_rx_words {
	u32 opts1;
	u32 addr;
	u32 opts2;
	u32 opts3;
};

/* struct odi_tx_words is the TX descriptor itself: the same five words the
 * hardware reads (opts1, addr, opts2, opts3, opts4), filled field by field
 * by the caller -- destination port mask and GEM stream id in opts3, CPU
 * tag priority and VLAN action in opts2, checksum and learning flags in
 * opts1.
 */
struct odi_tx_words {
	u32 opts1;
	u32 addr;
	u32 opts2;
	u32 opts3;
	u32 opts4;
};

/* Hook return codes: 0 STOP (consumed, free the skb),
 * 1 CONTINUE (next hook), 2 STOP_NOFREE (consumed, skb taken over).
 */
#define ODI_RXHOOK_STOP		0
#define ODI_RXHOOK_CONTINUE	1
#define ODI_RXHOOK_STOP_NOFREE	2

typedef int (*odi_rxhook_fn)(struct sk_buff *skb, const struct odi_rx_words *rx);

/* Returns 0 on success and -1 when full; the callers test for 0
 * (n13/n14: a void version left them a garbage status,
 * and the GPON module reported its OMCI hook registration as failed).
 */
int odi_nic_rxhook_register(int portmask, int priority, odi_rxhook_fn rx);
int odi_nic_rxhook_unregister(int portmask, int priority, odi_rxhook_fn rx);

/* A raw frame, its length, and the descriptor words the caller filled. */
int odi_nic_tx_words(const void *frame, unsigned short len, const struct odi_tx_words *tx);

#endif /* ODI_NIC_H */
