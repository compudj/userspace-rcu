// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * HPREF: Hazard pointers with reference counters
 */

#define _LGPL_SOURCE
#include <urcu/hpref.h>
#include <rseq/mempool.h>	/* Per-CPU memory */
#include <pthread.h>

static struct rseq_mempool *mempool;
static pthread_mutex_t hpref_sync_lock = PTHREAD_MUTEX_INITIALIZER;

struct hpref_percpu_slots *hpref_percpu_slots;
/*
 * The hpref period flips between 0 and 1 to guarantee forward
 * progress of hpref_synchronize(NULL) by preventing new readers
 * to keep a slot always active with the same value. Protected by
 * hpref_sync_lock.
 */
unsigned int hpref_period;

static
void hpref_release(struct urcu_ref *ref)
{
	struct hpref_node *node = caa_container_of(ref, struct hpref_node, refcount);

	node->release(node);
}

static
void hpref_scan_wait(struct hpref_node *node, unsigned int wait_period)
{
	int nr_cpus = rseq_get_max_nr_cpus(), cpu;

	/* Scan all CPUs slots. */
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		struct hpref_percpu_slots *cpu_slots = rseq_percpu_ptr(hpref_percpu_slots, cpu);
		struct hpref_slot *slot;
		unsigned int i;

		for (i = 0; i < HPREF_NR_PERCPU_SLOTS; i++) {
			slot = &cpu_slots->slots[i];
			if (!node) {	/* Wait for all HP. */
				struct hpref_node *slot_node = uatomic_load(&slot->node, CMM_ACQUIRE);

				/*
				 * Wait until either NULL or a slot node pointer value
				 * transition is observed for the current wait period.
				 * Either condition suffice to conclude that the slot
				 * has passed through a NULL state since the beginning
				 * of do_hpref_synchronize.
				 */
				if (!slot_node || (((uintptr_t) slot_node & 1) != wait_period))
					continue;
				while (uatomic_load(&slot->node, CMM_ACQUIRE) == slot_node)	/* Load B */
					caa_cpu_relax();
			} else {	/* Wait for a single HP value. */
				/* Busy-wait if node is found. For any period. */
				while (((uintptr_t) uatomic_load(&slot->node, CMM_ACQUIRE) & ~1UL) == (uintptr_t) node)	/* Load B */
					caa_cpu_relax();
			}
		}
	}
}

/*
 * hpref_synchronize: Wait for hazard pointer slots to be cleared.
 *
 * With a non-NULL @node argument, wait to observe that each slot
 * contains a value that differs from @node.
 *
 * With a NULL @node argument, wait to observe either NULL or a pointer
 * value transition for each slot, thus ensuring all pre-existing hazard
 * pointers were cleared at some point before completing the
 * synchronize.
 */
void hpref_synchronize(struct hpref_node *node)
{
	/* Memory ordering: Store A before Load B. */
	cmm_smp_mb();
	if (node) {
		/*
		 * Waiting for a single node can be done by scanning
		 * all slots for each period.
		 */
		hpref_scan_wait(node, 0);
	} else {
		int wait_period;

		/*
		 * Two-stages scan for readers prevents a steady stream
		 * of readers storing the same pointer value into the
		 * same slot to prevent forward progress of synchronize.
		 */
		pthread_mutex_lock(&hpref_sync_lock);
		wait_period = hpref_period ^ 1;
		hpref_scan_wait(NULL, wait_period);
		uatomic_store(&hpref_period, wait_period, CMM_RELAXED);
		hpref_scan_wait(NULL, wait_period ^ 1);
		pthread_mutex_unlock(&hpref_sync_lock);
	}
}

void hpref_node_put(struct hpref_node *node)
{
	if (!node)
		return;
	urcu_ref_put(&node->refcount, hpref_release);
}

/*
 * hpref_synchronize_put: Wait for any reader possessing a hazard
 *                        pointer to @node to clear its slot and
 *                        put reference count.
 */
void hpref_synchronize_put(struct hpref_node *node)
{
	if (!node)
		return;
	hpref_synchronize(node);
	hpref_node_put(node);
}

static __attribute__((constructor))
void hpref_init(void)
{
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
