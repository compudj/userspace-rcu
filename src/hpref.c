// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * HPREF: Hazard Pointers Protected Reference Counters
 */

#define _LGPL_SOURCE
#include <urcu/hpref.h>
#include <rseq/mempool.h>	/* Per-CPU memory */

static struct rseq_mempool *mempool;
struct hpref_slot *hpref_percpu_slots;

static
void hpref_release(struct urcu_ref *ref)
{
	struct hpref_node *node = caa_container_of(ref, struct hpref_node, refcount);

	node->release(node);
}

/*
 * hpref_synchronize: Wait for hazard pointer slots to be cleared.
 *
 * Wait to observe that each slot contains a value that differs from
 * @node.
 */
void hpref_synchronize(struct hpref_node *node)
{
	int nr_cpus = rseq_get_max_nr_cpus(), cpu;

	if (!node)
		return;
	/* Memory ordering: Store A before Load B. */
	cmm_smp_mb();
	/* Scan all CPUs slots. */
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		struct hpref_slot *slot = rseq_percpu_ptr(hpref_percpu_slots, cpu);

		/* Busy-wait if node is found. */
		while ((uatomic_load(&slot->node, CMM_ACQUIRE)) == node)	/* Load B */
			caa_cpu_relax();
	}
}

void hpref_refcount_dec(struct hpref_node *node)
{
	if (!node)
		return;
	urcu_ref_put(&node->refcount, hpref_release);
}

static __attribute__((constructor))
void hpref_init(void)
{
	mempool = rseq_mempool_create("hpref", sizeof(struct hpref_slot), NULL);
	if (!mempool)
		abort();
	hpref_percpu_slots = rseq_mempool_percpu_zmalloc(mempool);
	if (!hpref_percpu_slots)
		abort();
}

static __attribute__((destructor))
void hpref_exit(void)
{
	rseq_mempool_percpu_free(hpref_percpu_slots);
	if (rseq_mempool_destroy(mempool))
		abort();
}
