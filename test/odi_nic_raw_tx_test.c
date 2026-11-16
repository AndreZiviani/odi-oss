/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_nic_raw_tx_test.c -- the raw transmit path of odi_nic.c with the NIC
 * down. odi_omci.c sends through odi_nic_tx_words() -> odi_nic_xmit_raw()
 * with no net_device, so nothing but the driver stops it when every
 * interface is down and the rings are freed (tx_ring NULL).
 *
 * The functions odi_tx_full_locked, odi_nic_xmit_raw and odi_ndo_stop are
 * the driver's own text (cut out by the .sh); the kernel calls around them
 * are stubs that record what happened.
 *
 * Covers: a raw send with no ring returns -ENETDOWN and touches nothing; a
 * raw send with the ring up queues one frame; after the last stop the same
 * send is refused again; the last stop clears the flag before it cancels
 * work, frees the IRQ or frees the rings, and a stop that is not the last
 * leaves it set.
 */
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/extra/drivers/net/ethernet/odi/odi_nic_hw.h"

typedef uint32_t u32;
typedef uintptr_t dma_addr_t;
typedef int bool_t;

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

struct device { int x; };
static struct { struct device dev; } pdev, *odi_pdev = &pdev;
struct net_device { int x; };

static struct {
	int lock;
	unsigned int tx_tail, tx_reclaim, tx_queued;
	struct odi_tx_desc *tx_ring;
	void **tx_buf;
	dma_addr_t *tx_dma;
	unsigned int *tx_len;
	_Bool tx_up;
	int tx_backstop_work, tx_stall_work, state_dump_work, napi;
	int irq_requested;
} odi;

/* ---- kernel stubs ------------------------------------------------------- */

#define ODI_TX_BACKSTOP_MS 100
#define ODI_NIC_IRQ_NUM 7
#define GFP_ATOMIC 0
#define DMA_TO_DEVICE 0
#define spin_lock_irqsave(l, f) ((void)(l), (f) = 0)
#define spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
#define kmalloc(n, f) malloc(n)
#define kfree(p) free(p)
#define dma_map_single(d, p, n, dir) ((dma_addr_t)(p))
#define dma_mapping_error(d, a) 0
#define odi_dma_wmb() ((void)0)
#define odi_w32(a, v) ((void)0)
#define odi_r32(a) 0
#define schedule_delayed_work(w, t) ((void)(w))
#define msecs_to_jiffies(t) (t)
#define lockdep_assert_held(l) ((void)0)
#define atomic_dec_return(v) (--*(v))
#define netif_stop_queue(d) ((void)(d))
#define netif_carrier_off(d) ((void)0)

static int open_count;
#define odi_open_count open_count

/* Order of the teardown, with the flag as it stood at each step. */
static char steps[16][24];
static int nsteps;
static _Bool up_at[16];

static void step(const char *what)
{
	strcpy(steps[nsteps], what);
	up_at[nsteps++] = odi.tx_up;
}

#define cancel_delayed_work_sync(w) step("cancel")
#define napi_disable(n) step("napi_disable")
#define free_irq(i, d) step("free_irq")
static void odi_stop_hw(void) { step("stop_hw"); }
static void odi_rings_free(struct device *dev) { (void)dev; step("rings_free"); }

#include "odi_tx_full_locked.inc"
#include "odi_nic_xmit_raw.inc"
#include "odi_ndo_stop.inc"

/* ---- the test ----------------------------------------------------------- */

static sigjmp_buf segv_env;

static void on_segv(int sig)
{
	(void)sig;
	siglongjmp(segv_env, 1);
}

/* A send through a NULL ring faults; report it as a failure, not a crash. */
static int send_raw(void)
{
	static const unsigned char data[48];
	int rc;

	if (sigsetjmp(segv_env, 1))
		return 0x7fff;
	rc = odi_nic_xmit_raw(0, 0, 0, data, sizeof(data));
	return rc;
}

static void ring_up(void)
{
	odi.tx_ring = calloc(ODI_TX_RING_DEPTH, sizeof(*odi.tx_ring));
	odi.tx_buf = calloc(ODI_TX_RING_DEPTH, sizeof(*odi.tx_buf));
	odi.tx_dma = calloc(ODI_TX_RING_DEPTH, sizeof(*odi.tx_dma));
	odi.tx_len = calloc(ODI_TX_RING_DEPTH, sizeof(*odi.tx_len));
	odi.tx_tail = odi.tx_reclaim = odi.tx_queued = 0;
	odi.tx_up = 1;
}

static void ring_down(void)
{
	free(odi.tx_buf[0]);
	free(odi.tx_ring); free(odi.tx_buf); free(odi.tx_dma); free(odi.tx_len);
	odi.tx_ring = NULL; odi.tx_buf = NULL; odi.tx_dma = NULL; odi.tx_len = NULL;
}

int main(void)
{
	struct net_device dev = { 0 };
	int rc, i;

	signal(SIGSEGV, on_segv);
	signal(SIGBUS, on_segv);

	/* Never opened, or every interface down: tx_ring is NULL. */
	rc = send_raw();
	CHECK(rc != 0x7fff, "raw send with no ring does not touch the ring");
	CHECK(rc == -ENETDOWN, "raw send with no ring returns -ENETDOWN");
	CHECK(odi.tx_queued == 0 && odi.tx_tail == 0, "nothing queued with no ring");

	/* Ring up: one frame goes. */
	ring_up();
	rc = send_raw();
	CHECK(rc == 0, "raw send with the ring up succeeds");
	CHECK(odi.tx_queued == 1 && odi.tx_tail == 1, "the frame is queued");

	/* A stop that is not the last one leaves the flag alone. */
	open_count = 2;
	nsteps = 0;
	odi_ndo_stop(&dev);
	CHECK(odi.tx_up, "a stop that is not the last keeps the flag");
	CHECK(nsteps == 0, "a stop that is not the last tears nothing down");

	/* The last stop: flag first, then everything else. */
	open_count = 1;
	odi.irq_requested = 1;
	nsteps = 0;
	odi_ndo_stop(&dev);
	CHECK(nsteps >= 2 && !odi.tx_up, "the last stop clears the flag");
	for (i = 0; i < nsteps; i++)
		if (up_at[i])
			break;
	CHECK(i == nsteps, "the flag is already clear at every teardown step");
	CHECK(nsteps > 0 && !strcmp(steps[nsteps - 1], "rings_free"),
	      "the rings are freed last");
	ring_down();

	/* Ring gone: the same send is refused, not a NULL access. */
	rc = send_raw();
	CHECK(rc != 0x7fff, "raw send after the last stop does not touch the ring");
	CHECK(rc == -ENETDOWN, "raw send after the last stop returns -ENETDOWN");

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("odi_nic_raw_tx_test: ok\n");
	return 0;
}
