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

/*
 * Memory Ordering
 *
 * The variables accessed by the following memory operations are
 * categorized as:
 *
 *   A: Hazard pointers (HP).
 *   B: Hazard pointer slots.
 *   C: Per-CPU scan depth.
 *
 * A Dekker memory ordering [1] provides the hazard pointer existence
 * guarantee by ordering A vs B.
 *
 * The scan_depth is updated by both readers and HP synchronize. The
 * readers increase its value to cover the used slots range. HP
 * synchronize decreases its value to shrink the scan depth when it
 * encounters NULL pointers at the tail of the scanned slots range. This
 * value is increased/decreased by stride of 8 (aiming for cache line
 * size), with an hysteresis of 8 preventing the HP synchronize to
 * shrink the depth too aggressively.
 *
 * The scan depth is ordered with respect to slots with the following
 * two additional Dekker orderings: B vs C and C vs A. The B vs C Dekker
 * [2] orders decrease of the scan_depth by synchronize, whereas the
 * C vs A [3] Dekker orders increase of the scan_depth by readers.
 *
 * * Readers
 *   - Load node ptr
 *   - Store pointer to slot N          (Store B)
 *   - Memory barrier (smp_mb())        (Order Store B before Load A [1])
 *                                      (Order Store B before Load C [2])
 *   - Load scan_depth                  (Load C)
 *     - If scan_depth does not cover N, increase it to the
 *       next multiple of 8 >= N with a cmpxchg until its
 *       value covers N.                (Store C)
 *       - cmpxchg memory barrier       (Order Store C before Load A [3])
 *    - Re-load node ptr                (Load A)
 *
 * * HP synchronize (single synchronize protected by locking)
 *   - Caller unpublish A               (Store A)
 *   - Memory barrier (smp_mb())        (Order Store A before load C [3])
 *                                      (Order Store A before load B [1])
 -   - Load scan_depth                  (Load C)
 *   - Iterate on all slots up to scan_depth, noting the
 *     position (pos) of the last node that was encountered as
 *     non-NULL.                        (Load B)
 *   - If pos < scan_depth - 8, update scan_depth to the next
 *     multiple of 8 >= pos (new_depth) with an xchg. The old
 *     value returned by xchg is old_depth.
 *                                      (Store C)
 *   - Memory barrier (smp_mb())        (Order Store C before Load B [2])
 *   - Iterate on all slots between new_depth and old_depth.
 *     Load each slot.                  (Load B)
 *     Note the position of the last non-NULL slot (if any).
 *   - Increase the scan_depth to the next multiple of 8 >= last
 *     non-NULL slot position with cmpxchg until its value
 *     covers the last non-NULL slot.
 */
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
void hpref_scan_wait(void *addr, size_t length, unsigned int wait_period)
{
	int nr_cpus = rseq_get_max_nr_cpus(), cpu;

	/* Scan all CPUs slots. */
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		struct hpref_percpu_slots *cpu_slots = rseq_percpu_ptr(hpref_percpu_slots, cpu);
		unsigned long *scan_depth_p, scan_depth;
		unsigned long last_used_pos = 0, i;

		scan_depth_p = hpref_get_cpu_slots_scan_depth(cpu_slots);
		scan_depth = uatomic_load(scan_depth_p, CMM_RELAXED);
		for (i = HPREF_FIRST_SCAN_SLOT; i < scan_depth; i++) {
			struct hpref_slot *slot = &cpu_slots->slots[i];

			if (!addr || length > sizeof(struct hpref_node)) {
				struct hpref_node *slot_node = uatomic_load(&slot->node, CMM_ACQUIRE);

				if (slot_node)
					last_used_pos = i;
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
				struct hpref_node *slot_node = uatomic_load(&slot->node, CMM_ACQUIRE);	/* Load B */

				if (slot_node)
					last_used_pos = i;
				while (((uintptr_t) slot_node & ~1UL) == (uintptr_t) addr) {	/* Load B */
					caa_cpu_relax();
					slot_node = uatomic_load(&slot->node, CMM_ACQUIRE);
				}
			}
		}
		/* Decrease the scan depth if needed. */
		if (last_used_pos + HPREF_SHRINK_HYSTERESIS < scan_depth) {
			unsigned long old_depth, new_depth = HPREF_ALIGN(last_used_pos + 1, HPREF_DEPTH_STRIDE);

			/* Tentatively reduce the scan depth. */
			old_depth = uatomic_xchg(scan_depth_p, new_depth);	/* Store C */
			/* Memory ordering: Store C before Load B. Pairs with cmm_barrier(). */
			if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0))
				urcu_die(errno);
			/*
			 * Scan the removed range to validate whether it contains any
			 * non-NULL slots.
			 */
			last_used_pos = 0;
			for (i = new_depth - 1; i < old_depth; i++) {
				struct hpref_slot *slot = &cpu_slots->slots[i];
				struct hpref_node *slot_node = uatomic_load(&slot->node, CMM_ACQUIRE); /* Load B */

				if (slot_node)
					last_used_pos = i;
			}
			/*
			 * If non-NULL slots are found in the range removed from scan depth,
			 * increase the scan depth to cover those slots.
			 */
			if (new_depth < last_used_pos + 1) {
				new_depth = HPREF_ALIGN(last_used_pos + 1, HPREF_DEPTH_STRIDE);

				for (;;) {
					old_depth = uatomic_cmpxchg(scan_depth_p, scan_depth, new_depth);
					if (old_depth == scan_depth || old_depth >= new_depth)
						break;
					scan_depth = old_depth;
				}
			}
		}
	}
}

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
void hpref_synchronize(void *addr, size_t length)
{
	/* Memory ordering: Store A before Load B. */
	cmm_smp_mb();
	pthread_mutex_lock(&hpref_sync_lock);
	if (addr && length <= sizeof(struct hpref_node)) {
		/*
		 * Waiting for a single node can be done by scanning
		 * all slots for each period.
		 */
		hpref_scan_wait(addr, length, 0);
	} else {
		int wait_period;

		/*
		 * Two-stages scan for readers prevents a steady stream
		 * of readers storing the same pointer value into the
		 * same slot to prevent forward progress of synchronize.
		 */
		wait_period = hpref_period ^ 1;
		hpref_scan_wait(addr, length, wait_period);
		uatomic_store(&hpref_period, wait_period, CMM_RELAXED);
		hpref_scan_wait(addr, length, wait_period ^ 1);
	}
	pthread_mutex_unlock(&hpref_sync_lock);
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
	hpref_synchronize(node, sizeof(struct hpref_node));
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
