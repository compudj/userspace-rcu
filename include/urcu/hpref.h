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
#include <stdbool.h>
#include <rseq/mempool.h>	/* Per-CPU memory */
#include <rseq/rseq.h>

#include <urcu/ref.h>
#include <urcu/uatomic.h>
#include <urcu/compiler.h>

struct hpref_node {
	struct urcu_ref refcount;
	void (*release)(struct hpref_node *node);
};

struct hpref_slot {
	/* Use rseq to set from reader only if zero. */
	struct hpref_node *node;
};

#define NR_PERCPU_SLOTS_BITS	3
#define HPREF_NR_PERCPU_SLOTS	(1U << NR_PERCPU_SLOTS_BITS)
/*
 * The emergency slot is only used for short critical sections
 * (would be preempt off in when porting this code to the kernel): only
 * to ensure we have a free slot for taking a reference count as
 * fallback.
 */
#define HPREF_EMERGENCY_SLOT	(HPREF_NR_PERCPU_SLOTS - 1)

struct hpref_percpu_slots {
	struct hpref_slot slots[HPREF_NR_PERCPU_SLOTS];
	unsigned int current_slot;
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

/*
 * hpref_hp_get: Obtain a reference to a stable object, protected either
 *               by hazard pointer (fast-path) or using reference
 *               counter as fall-back.
 */
static inline
bool hpref_hp_get(struct hpref_node **node_p, struct hpref_ctx *ctx)
{
	int cpu = rseq_current_cpu_raw();
	struct hpref_percpu_slots *cpu_slots = rseq_percpu_ptr(hpref_percpu_slots, cpu);
	struct hpref_slot *slot = &cpu_slots->slots[cpu_slots->current_slot];
	bool use_refcount = false;
	struct hpref_node *node, *node2;
	unsigned int next_slot;

retry:
	node = uatomic_load(node_p, CMM_RELAXED);
	if (!node)
		return false;
	/* Use rseq to try setting current slot hp. Store B. */
	if (rseq_load_cbne_store__ptr(RSEQ_MO_RELAXED, RSEQ_PERCPU_CPU_ID,
				(intptr_t *) &slot->node, (intptr_t) NULL,
				(intptr_t) node, cpu)) {
		slot = &cpu_slots->slots[HPREF_EMERGENCY_SLOT];
		use_refcount = true;
		/*
		 * This may busy-wait for another reader using the
		 * emergency slot to transition to refcount.
		 */
		caa_cpu_relax();
		goto retry;
	}
	/* Memory ordering: Store B before Load A. Pairs with membarrier. */
	cmm_barrier();
	node2 = uatomic_load(node_p, CMM_RELAXED);	/* Load A */
	if (node != node2) {
		uatomic_store(&slot->node, NULL, CMM_RELAXED);
		if (!node2)
			return false;
		goto retry;
	}
	ctx->type = HPREF_TYPE_HP;
	ctx->hp = node;
	ctx->slot = slot;
	if (use_refcount) {
		hpref_promote_hp_to_ref(ctx);
		return true;
	}
	/*
	 * Increment current slot (racy increment is OK because it is
	 * just a position hint). Skip the emergency slot.
	 */
	next_slot = uatomic_load(&cpu_slots->current_slot, CMM_RELAXED) + 1;
	if (next_slot >= HPREF_EMERGENCY_SLOT)
		next_slot = 0;
	uatomic_store(&cpu_slots->current_slot, next_slot, CMM_RELAXED);
	return true;
}

static
void hpref_release(struct urcu_ref *ref)
{
	struct hpref_node *node = caa_container_of(ref, struct hpref_node, refcount);

	node->release(node);
}

static inline
void hpref_put(struct hpref_ctx *ctx)
{
	if (ctx->type == HPREF_TYPE_REF) {
		urcu_ref_put(&ctx->hp->refcount, hpref_release);
	} else {
		/* Release HP. */
		uatomic_store(&ctx->slot->node, NULL, CMM_RELEASE);
	}
	ctx->hp = NULL;
}

/*
 * hpref_set_pointer: Store pointer @node to @ptr, with RCU publication
 *                    guarantees.
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

/*
 * hpref_synchronize: Wait for any reader possessing a hazard pointer to
 *                    @node to clear its hazard pointer slot.
 */
void hpref_synchronize(struct hpref_node *node);

/*
 * hpref_synchronize_put: Wait for any reader possessing a hazard
 *                        pointer to clear its slot and put reference
 *                        count.
 */
void hpref_synchronize_put(struct hpref_node *node);

#ifdef __cplusplus
}
#endif

#endif /* _URCU_HPREF_H */
