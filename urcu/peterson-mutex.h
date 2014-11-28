#ifndef _URCU_PETERSON_MUTEX_H
#define _URCU_PETERSON_MUTEX_H

/*
 * urcu/peterson-mutex.h
 *
 * Userspace RCU library - Peterson Mutex
 *
 * Copyright 2014 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * 2-class mutex based on Peterson's algorithm. There needs to be only a
 * single thread in the "single fast" class, but the "multi slow" class
 * supports many concurrent threads. This is especially well suited for
 * locking data structures which are frequently accessed from a single
 * thread, and infrequently accessed from other threads (e.g. per-cpu
 * data, workqueues with work stealing...).
 *
 * The "single fast" class does not require any complex atomic
 * operations, only stores and loads. The "multi slow" class is more
 * heavy weight, and needs an atomic cmpxchg.
 *
 * This implementation does not guarantee that unlock/lock pairs act as
 * full memory barrier (no transitivity).
 *
 * Note: fast-path memory barriers could be turned into compiler
 * barriers if sys_membarrier() makes its way into the kernel.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu/arch.h>
#include <urcu/syscall-compat.h>

//#define SYS_membarrier		321	// XXX: TEST

#ifdef SYS_membarrier

enum {
	/* Expedited: adds some overhead, fast execution (few microseconds) */
	MEMBARRIER_EXPEDITED = (1 << 0),
	/* Delayed: Low overhead, but slow execution (few milliseconds) */
	MEMBARRIER_DELAYED = (1 << 1),
	/* Query flag support, without performing synchronization */
	MEMBARRIER_QUERY = (1 << 16),
};

#define membarrier(...)                syscall(SYS_membarrier, __VA_ARGS__)

static inline void membarrier_smp_wmb_fast(void)
{
	cmm_barrier();
}

static inline void membarrier_smp_rmb_fast(void)
{
	cmm_barrier();
}

static inline void membarrier_smp_mb_fast(void)
{
	cmm_barrier();
}

static inline void membarrier_smp_wmb_slow(void)
{
	(void) membarrier(MEMBARRIER_EXPEDITED);
}

static inline void membarrier_smp_rmb_slow(void)
{
	(void) membarrier(MEMBARRIER_EXPEDITED);
}

static inline void membarrier_smp_mb_slow(void)
{
	(void) membarrier(MEMBARRIER_EXPEDITED);
}

static inline void membarrier_after_implicit_mb_slow(void)
{
	(void) membarrier(MEMBARRIER_EXPEDITED);
}

#else

static inline void membarrier_smp_wmb_fast(void)
{
	cmm_smp_wmb();
}

static inline void membarrier_smp_rmb_fast(void)
{
	cmm_smp_rmb();
}

static inline void membarrier_smp_mb_fast(void)
{
	cmm_smp_mb();
}

static inline void membarrier_smp_wmb_slow(void)
{
	cmm_smp_wmb();
}

static inline void membarrier_smp_rmb_slow(void)
{
	cmm_smp_rmb();
}

static inline void membarrier_smp_mb_slow(void)
{
	cmm_smp_mb();
}

static inline void membarrier_after_implicit_mb_slow(void)
{
}

#endif	/* #ifdef SYS_membarrier */

struct urcu_peterson_mutex {
	/* flag[0] is single-thread fast path flag. */
	/* flag[1] is multi-thread slow path flag. */
	int flag[2];
	int turn;
};

/*
 * Call from a single thread, ideally the thread accessing the mutex the most
 * frequently.
 */
static inline
void urcu_pt_mutex_lock_single_fast(struct urcu_peterson_mutex *pm)
{
	CMM_STORE_SHARED(pm->flag[0], 1);
	/* Store flag[0] before store turn. */
	membarrier_smp_wmb_fast();
	CMM_STORE_SHARED(pm->turn, 1);
	/* Store flag[0], turn before load flag[1]. */
	membarrier_smp_mb_fast();
	while (CMM_LOAD_SHARED(pm->flag[1]) && CMM_LOAD_SHARED(pm->turn) == 1)
		caa_cpu_relax();	/* busy wait */
	/*
	 * Control dependency orders flag[1], turn loads against later
	 * stores.
	 */
	/* load flag[1], turn before c.s. loads. */
	membarrier_smp_rmb_fast();
	/* Critical section. */
}

static inline
void urcu_pt_mutex_unlock_single_fast(struct urcu_peterson_mutex *pm)
{
	/* End of critical section. */
	/* c.s. does not leak out of store to flag[0]. */
	membarrier_smp_mb_fast();
	CMM_STORE_SHARED(pm->flag[0], 0);
}

/*
 * Call from multiple concurrent threads. Slow path using lock prefix.
 */
static inline
void urcu_pt_mutex_lock_multi_slow(struct urcu_peterson_mutex *pm)
{
	int ret;

	/* Busy-wait on getting slow path flag. */
	do {
		ret = uatomic_cmpxchg(&pm->flag[1], 0, 1);
	} while (ret != 0);
	/* Store flag[1] before store turn, implicit by cmpxchg. */
	membarrier_after_implicit_mb_slow();
	CMM_STORE_SHARED(pm->turn, 0);
	/* Store flag[1], turn before load flag[0]. */
	membarrier_smp_mb_slow();
	while (CMM_LOAD_SHARED(pm->flag[0]) && CMM_LOAD_SHARED(pm->turn) == 0)
		caa_cpu_relax();	/* busy wait */
	/*
	 * Control dependency orders flag[0], turn loads against later
	 * stores.
	 */
	/* load flag[0], turn before c.s. loads. */
	membarrier_smp_rmb_slow();
	/* Critical section */
}

static inline
void urcu_pt_mutex_unlock_multi_slow(struct urcu_peterson_mutex *pm)
{
	/* End of critical section. */
	/* c.s. does not leak out of store to flag[1]. */
	membarrier_smp_mb_slow();
	CMM_STORE_SHARED(pm->flag[1], 0);
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_PETERSON_MUTEX_H */
