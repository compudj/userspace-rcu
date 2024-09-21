// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _URCU_HPREF_H
#define _URCU_HPREF_H

/*
 * HPREF: Hazard pointers with reference counters
 *
 * This API combines hazard pointers and reference counters.
 * It uses hazard pointers as fast-paths, and fall-back to reference
 * counters either explicitly when the reader expects to hold the object
 * for a long time, or when no hazard pointer slots are available.
 *
 * This leverages the fact that both synchronization mechanisms aim to
 * guarantee existence of objects, and those existence guarantees can be
 * chained. Each mechanism achieves its purpose in a different way with
 * different tradeoffs. The hazard pointers are faster to read and scale
 * better than reference counters, but they consume more memory than a
 * per-object reference counter.
 *
 * The fall-back to reference counter allows bounding the number of
 * hazard pointer slots to a fixed size for the entire system:
 * nr_cpus * N, where N=8 as it fills a single 64 bytes cache line on
 * 64-bit architectures.
 *
 * References:
 *
 * [1]: M. M. Michael, "Hazard pointers: safe memory reclamation for
 *      lock-free objects," in IEEE Transactions on Parallel and
 *      Distributed Systems, vol. 15, no. 6, pp. 491-504, June 2004
 */

#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <rseq/mempool.h>	/* Per-CPU memory */
#include <rseq/rseq.h>

#include <urcu/ref.h>
#include <urcu/uatomic.h>
#include <urcu/compiler.h>
#include <urcu/pointer.h>

struct hpref_node {
	struct urcu_ref refcount;
	void (*release)(struct hpref_node *node);
};

struct hpref_slot {
	/* Use rseq to set from reader only if zero. */
	struct hpref_node *node;
};

/*
 * Allocate 64 slots per CPU. It fits within 256 bytes on 32-bit, 512
 * bytes on 64-bit.
 * The scan depth dynamically ajusts the depth of the hazard pointer
 * scan based on reader slots usage.
 *
 * The layout of the per-CPU slots area is as follows:
 * - slots (64 * sizeof(struct hpref_slot))
 *  - The first slot is reserved to hold the scan_depth value, so the
 *    scan_depth is located on the same cache line as the first slots.
 *  - The last slot is an emergency reserve.
 */
#define HPREF_NR_PERCPU_SLOTS_BITS	6
#define HPREF_NR_PERCPU_SLOTS		(1U << HPREF_NR_PERCPU_SLOTS_BITS)
/* Scan skips the first slot (scan_depth). */
#define HPREF_FIRST_SCAN_SLOT		1
/*
 * The emergency reserve slot is only used internally for short critical
 * sections (would be preempt off in when porting this code to the
 * kernel). It ensures there is a free slot to guarantee hpref_node
 * existence to immediately promote to a reference count as fallback.
 */
#define HPREF_EMERGENCY_SLOT		(HPREF_NR_PERCPU_SLOTS - 1)

#define HPREF_DEPTH_STRIDE_BITS		3
#define HPREF_DEPTH_STRIDE		(1U << HPREF_DEPTH_STRIDE_BITS)
#define HPREF_SHRINK_HYSTERESIS		HPREF_DEPTH_STRIDE
#define HPREF_ALIGN(depth, align)	((depth) + (align - 1)) & (~(align - 1));

struct hpref_percpu_slots {
	struct hpref_slot slots[HPREF_NR_PERCPU_SLOTS];
};

enum hpref_type {
	HPREF_TYPE_HP,
	HPREF_TYPE_REF,
};

struct hpref_ctx {
	struct hpref_slot *slot;
	struct hpref_node *hp;
	enum hpref_type type;
};

#ifdef __cplusplus
extern "C" {
#endif

extern struct hpref_percpu_slots *hpref_percpu_slots;
/*
 * hpref_period is used for slot pointer tagging to guarantee
 * forward progress of synchronize.
 */
extern unsigned int hpref_period;

/*
 * hpref_synchronize: Wait for hazard pointer slots to be cleared.
 *
 * With a non-NULL @node argument, wait to observe that each slot
 * contains a value that is not within the range:
 *
 *  [addr, addr + length - 1].
 *
 * With a NULL @node argument, wait to observe either NULL or a pointer
 * value transition for each slot, thus ensuring all pre-existing hazard
 * pointers were cleared at some point before completing the
 * synchronize.
 */
void hpref_synchronize(void *addr, size_t length);

/*
 * Put node reference count. Should be used after synchronize.
 */
void hpref_node_put(struct hpref_node *node);

/*
 * hpref_synchronize_put: Wait for any reader possessing a hazard
 *                        pointer to clear its slot and put reference
 *                        count.
 */
void hpref_synchronize_put(struct hpref_node *node);

static inline
void hpref_node_init(struct hpref_node *node,
		void (*release)(struct hpref_node *node))
{
	urcu_ref_init(&node->refcount);
	node->release = release;
}

/*
 * hpref_promote_hp_to_ref: Promote hazard pointer to reference count.
 */
static inline
void hpref_promote_hp_to_ref(struct hpref_ctx *ctx)
{
	if (ctx->type == HPREF_TYPE_REF)
		return;
	urcu_ref_get(&ctx->hp->refcount);
	uatomic_store(&ctx->slot->node, NULL, CMM_RELEASE);
	ctx->slot = NULL;
	ctx->type = HPREF_TYPE_REF;
}

static inline
unsigned long *hpref_get_cpu_slots_scan_depth(struct hpref_percpu_slots *cpu_slots)
{
	return (unsigned long *) &cpu_slots->slots[0];
}

/*
 * hpref_hp_get: Obtain a reference to a stable object, protected either
 *               by hazard pointer (fast-path) or using reference
 *               counter as fall-back.
 * Return values: 0 if hazard pointer is NULL, 1 otherwise.
 */
static inline
int hpref_hp_get(struct hpref_node **node_p, struct hpref_ctx *ctx)
{
	int cpu = rseq_current_cpu_raw(), ret;
	struct hpref_percpu_slots *cpu_slots = rseq_percpu_ptr(hpref_percpu_slots, cpu);
	unsigned int period = uatomic_load(&hpref_period, CMM_RELAXED);
	unsigned long *scan_depth_p, scan_depth;
	struct hpref_node *node, *node2;
	struct hpref_slot *slot;
	unsigned int slot_nr;

	node = uatomic_load(node_p, CMM_RELAXED);
	if (!node)
		return 0;
retry:
	for (slot_nr = HPREF_FIRST_SCAN_SLOT;
			slot_nr < HPREF_NR_PERCPU_SLOTS;
			/* inc in loop. */) {
		slot = &cpu_slots->slots[slot_nr];
		/* Use rseq to try setting slot hp. Store B. */
		ret = rseq_load_cbne_store__ptr(RSEQ_MO_RELAXED, RSEQ_PERCPU_CPU_ID,
					(intptr_t *) &slot->node, (intptr_t) NULL,
					(intptr_t) node | period, cpu);
		if (!ret)
			break;	/* Success. */
		if (ret < 0) {
			/*
			 * Abort due to preemption/migration/signal
			 * delivery or CPU number mismatch.
			 */
			cpu = rseq_current_cpu_raw();
			cpu_slots = rseq_percpu_ptr(hpref_percpu_slots, cpu);
		}
		if (slot_nr == HPREF_EMERGENCY_SLOT) {
			/*
			 * This may busy-wait for another reader using the
			 * emergency slot to transition to refcount.
			 */
			caa_cpu_relax();
		} else {
			slot_nr++;
		}
		goto retry;
	}

	/* Memory ordering: Store B before Load A. Pairs with membarrier. */
	/* Memory ordering: Store B before Load C. Pairs with membarrier. */
	cmm_barrier();

	/* Increase scan depth if needed. */
	scan_depth_p = hpref_get_cpu_slots_scan_depth(cpu_slots);
	scan_depth = uatomic_load(scan_depth_p, CMM_RELAXED);	/* Load C */
	if (caa_unlikely(scan_depth < slot_nr + 1)) {
		unsigned long old_depth, new_depth = HPREF_ALIGN(slot_nr + 1, HPREF_DEPTH_STRIDE);

		for (;;) {
			/* Memory ordering: Store C before Load A ordering provided by cmpxchg. */
			old_depth = uatomic_cmpxchg(scan_depth_p, scan_depth, new_depth);	/* Store C */
			if (old_depth == scan_depth || old_depth >= new_depth)
				break;
			scan_depth = old_depth;
		}
	}

	node2 = rcu_dereference(*node_p);	/* Load A */
	/*
	 * If @node_p content has changed since the first load,
	 * clear the hazard pointer and try again.
	 * Comparing node and node2 pointers is fine because this
	 * function issues both loads, and therefore the comparison is
	 * not against a user-controlled pointer value which may be
	 * known at compile-time. This prevents compiler optimizations
	 * that would affect ordering.
	 */
	if (node != node2) {
		uatomic_store(&slot->node, NULL, CMM_RELAXED);
		if (!node2)
			return 0;
		node = node2;
		goto retry;
	}
	ctx->type = HPREF_TYPE_HP;
	ctx->hp = node2;
	ctx->slot = slot;
	if (slot_nr == HPREF_EMERGENCY_SLOT)
		hpref_promote_hp_to_ref(ctx);
	return 1;
}

/*
 * Put reader context reference count.
 */
static inline
void hpref_put(struct hpref_ctx *ctx)
{
	if (ctx->type == HPREF_TYPE_REF) {
		hpref_node_put(ctx->hp);
	} else {
		/* Release HP. */
		uatomic_store(&ctx->slot->node, NULL, CMM_RELEASE);
	}
	ctx->hp = NULL;
}

/*
 * hpref_set_pointer: Store pointer @node to @ptr, with store-release MO.
 */
static inline
void hpref_set_pointer(struct hpref_node **ptr, struct hpref_node *node)
{
	if (__builtin_constant_p(node) && node == NULL)
		uatomic_store(ptr, NULL, CMM_RELAXED);
	else
		uatomic_store(ptr, node, CMM_RELEASE);
}

/*
 * Return the content of the hpref context hazard pointer field.
 */
static inline
struct hpref_node *hpref_ctx_pointer(struct hpref_ctx *ctx)
{
	return ctx->hp;
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_HPREF_H */
