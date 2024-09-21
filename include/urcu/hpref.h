// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _URCU_HPREF_H
#define _URCU_HPREF_H

/*
 * HPREF: Hazard Pointers Protected Reference Counters
 *
 * This API internally uses hazard pointers to provide existence
 * guarantees of objects, and promotes this to a reference count
 * increment before returning the object.
 *
 * This leverages the fact that both synchronization mechanisms aim to
 * guarantee existence of objects, and those existence guarantees can be
 * chained. Each mechanism achieves its purpose in a different way with
 * different tradeoffs. The hazard pointers are faster to read and scale
 * better than reference counters, but they consume more memory than a
 * per-object reference counter.
 *
 * This API uses a fixed number of hazard pointer slots (nr_cpus) across
 * the entire system.
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

/* Per-CPU hazard pointer slot. */
struct hpref_slot {
	/* Use rseq to set from reader only if zero. */
	struct hpref_node *node;
};

#ifdef __cplusplus
extern "C" {
#endif

extern struct hpref_slot *hpref_percpu_slots;

/*
 * hpref_synchronize: Wait for hazard pointer slots to be cleared.
 *
 * Wait to observe that each slot contains a value that differs from
 * @node. When hpref_hp_refcount_inc() is used concurrently to
 * dereference a pointer to a node, at least one hpref_synchronize() for
 * that node should complete between the point where all pointers to the
 * node observable by hpref_hp_refcount_inc() are unpublished and the
 * hpref_refcount_dec() associated with the node's initial reference.
 */
void hpref_synchronize(struct hpref_node *node);

/*
 * Decrement node reference count, execute release callback when
 * reaching 0.
 */
void hpref_refcount_dec(struct hpref_node *node);

/*
 * Initialize a hpref_node.
 */
static inline
void hpref_node_init(struct hpref_node *node,
		void (*release)(struct hpref_node *node))
{
	urcu_ref_init(&node->refcount);
	node->release = release;
}

/*
 * hpref_hp_refcount_inc: Obtain a reference to an object.
 *
 * Protected by hazard pointer internally, chained with increment of a
 * reference count. Returns a pointer to an object or NULL. If
 * the returned node is not NULL, the node is guaranteed to exist and
 * the caller owns a reference count to the node.
 */
static inline
struct hpref_node *hpref_hp_refcount_inc(struct hpref_node **node_p)
{
	int cpu = rseq_current_cpu_raw(), ret;
	struct hpref_slot *slot = rseq_percpu_ptr(hpref_percpu_slots, cpu);
	struct hpref_node *node, *node2;

	node = uatomic_load(node_p, CMM_RELAXED);
	if (!node)
		return NULL;
retry:
	/* Use rseq to try setting slot hp. Store B. */
	ret = rseq_load_cbne_store__ptr(RSEQ_MO_RELAXED, RSEQ_PERCPU_CPU_ID,
				(intptr_t *) &slot->node, (intptr_t) NULL,
				(intptr_t) node, cpu);
	if (ret) {
		if (ret < 0) {
			/*
			 * Abort due to preemption/migration/signal
			 * delivery or CPU number mismatch.
			 */
			cpu = rseq_current_cpu_raw();
			slot = rseq_percpu_ptr(hpref_percpu_slots, cpu);
		}
		/*
		 * Busy-wait for another reader using the emergency slot
		 * to transition to refcount.
		 */
		caa_cpu_relax();
		goto retry;
	}
	/* Memory ordering: Store B before Load A. */
	cmm_smp_mb();
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
			return NULL;
		node = node2;
		goto retry;
	}
	/* Promote to reference count. */
	urcu_ref_get(&node2->refcount);
	/* Release hazard pointer. */
	uatomic_store(&slot->node, NULL, CMM_RELEASE);
	return node2;
}

/*
 * hpref_set_pointer: Store pointer @node to @ptr, with store-release MO.
 */
static inline
void hpref_set_pointer(struct hpref_node **ptr, struct hpref_node *node)
{
	rcu_set_pointer(ptr, node);	/* Store A */
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_HPREF_H */
