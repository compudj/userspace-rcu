// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * HPREF: Hazard pointers with reference counters
 */

#define _LGPL_SOURCE
#include <urcu/hpref.h>
#include <sys/syscall.h>	/* Definition of SYS_* constants */
#include <unistd.h>
#include "urcu-die.h"
#include <rseq/mempool.h>	/* Per-CPU memory */

#define membarrier(...)		syscall(__NR_membarrier, __VA_ARGS__)

enum membarrier_cmd {
	MEMBARRIER_CMD_QUERY				= 0,
	MEMBARRIER_CMD_SHARED				= (1 << 0),
	/* reserved for MEMBARRIER_CMD_SHARED_EXPEDITED (1 << 1) */
	/* reserved for MEMBARRIER_CMD_PRIVATE (1 << 2) */
	MEMBARRIER_CMD_PRIVATE_EXPEDITED		= (1 << 3),
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED	= (1 << 4),
};

static struct rseq_mempool *mempool;
struct hpref_percpu_slots *hpref_percpu_slots;

/*
 * hpref_synchronize: Wait for any reader possessing a hazard pointer to
 *                    @node to clear its hazard pointer slot.
 */
void hpref_synchronize(struct hpref_node *node)
{
	int nr_cpus = rseq_get_max_nr_cpus(), cpu = 0;

	/* Memory ordering: Store A before Load B. Pairs with cmm_barrier(). */
	if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0))
		urcu_die(errno);
	/* Scan all CPUs slots. */
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		struct hpref_percpu_slots *cpu_slots = rseq_percpu_ptr(hpref_percpu_slots, cpu);
		struct hpref_slot *slot;
		unsigned int i;

		for (i = 0; i < HPREF_NR_PERCPU_SLOTS; i++) {
			slot = &cpu_slots->slots[i];
			/* Busy-wait if node is found. */
			while (uatomic_load(&slot->node, CMM_ACQUIRE) == node)	/* Load B */
				caa_cpu_relax();
		}
	}
}

/*
 * hpref_synchronize_put: Wait for any reader possessing a hazard
 *                        pointer to clear its slot and put reference
 *                        count.
 */
void hpref_synchronize_put(struct hpref_node *node)
{
	if (!node)
		return;
	hpref_synchronize(node);
	urcu_ref_put(&node->refcount, hpref_release);
}

static __attribute__((constructor))
void hpref_init(void)
{
	if (membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0))
		urcu_die(errno);
	mempool = rseq_mempool_create("hpref", sizeof(struct hpref_percpu_slots), NULL);
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
