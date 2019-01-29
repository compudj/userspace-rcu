#ifndef _URCU_PERCPU_STATIC_H
#define _URCU_PERCPU_STATIC_H

/*
 * urcu-percpu-static.h
 *
 * Userspace RCU percpu header.
 *
 * TO BE INCLUDED ONLY IN CODE THAT IS TO BE RECOMPILED ON EACH LIBURCU
 * RELEASE. See urcu.h for linking dynamically with the userspace rcu library.
 *
 * Copyright (c) 2009, 2016 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
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
 *
 * IBM's contributions to this file may be relicensed under LGPLv2 or later.
 */

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sched.h>

#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <urcu/system.h>
#include <urcu/uatomic.h>
#include <urcu/futex.h>
#include <urcu/tls-compat.h>
#include <urcu/rseq.h>
#include <urcu/debug.h>
#include <urcu/rseq.h>
#include <cpu-op.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This code section can only be included in LGPL 2.1 compatible source code.
 * See below for the function call wrappers which can be used in code meant to
 * be only linked with the Userspace RCU library. This comes with a small
 * performance degradation on the read-side due to the added function calls.
 * This is required to permit relinking with newer versions of the library.
 */

/*
 * Slave barriers are only guaranteed to be ordered wrt master barriers.
 *
 * The pair ordering is detailed as (O: ordered, X: not ordered) :
 *               slave  master
 *        slave    X      O
 *        master   O      O
 */

extern int urcu_percpu_has_sys_membarrier;
extern __thread int srcu_state;

int urcu_percpu_current_cpu(void);

static inline void smp_mb_slave(void)
{
	if (caa_likely(urcu_percpu_has_sys_membarrier))
		cmm_barrier();
	else
		cmm_smp_mb();
}

#define RCU_GP_CTR_PHASE	(1UL << 0)

struct rcu_gp {
	/*
	 * Global gp phrase (0/1).
	 * Written to only by writer with mutex taken.
	 * Read by both writer and readers.
	 */
	int ctr;

	int32_t futex;
} __attribute__((aligned(CAA_CACHE_LINE_SIZE)));

extern struct rcu_gp rcu_gp;

struct rcu_percpu_count {
	uintptr_t lock;
	uintptr_t unlock;
};

#define RCU_PERCPU_ARRAY_COUNT	2

struct rcu_percpu {
	struct rcu_percpu_count count[RCU_PERCPU_ARRAY_COUNT];
} __attribute__((aligned(CAA_CACHE_LINE_SIZE)));

struct rcu_cpus {
	struct rcu_percpu *p;
};

struct rcu_cpus rcu_cpus;

/*
 * Wake-up waiting synchronize_rcu(). Called from many concurrent threads.
 */
static inline void wake_up_gp(void)
{
	if (caa_unlikely(uatomic_read(&rcu_gp.futex) == -1)) {
		uatomic_set(&rcu_gp.futex, 0);
		/*
		 * Ignoring return value until we can make this function
		 * return something (because urcu_die() is not publicly
		 * exposed).
		 */
		(void) futex_async(&rcu_gp.futex, FUTEX_WAKE, 1,
				NULL, NULL, 0);
	}
}

static inline void _rcu_inc_lock(unsigned int period)
{
	int cpu, ret;

retry:
	/* rseq fast-path. */
	cpu = rseq_cpu_start();
	ret = rseq_addv((intptr_t *)&rcu_cpus.p[cpu].count[period].lock,
			1, cpu);
	if (caa_likely(!ret)) {
		cmm_barrier();
		return;
	}
	/* rseq has either been aborted, or is not initialized. */
	cpu = rseq_current_cpu_raw();
	if (cpu < 0) {
		if (cpu == -1 && !urcu_rseq_register_current_thread())
			goto retry;
		goto norseq_fallback;
	}
	/* rseq has been aborted, use cpu_opv. */
	for (;;) {
		cpu = urcu_percpu_current_cpu();
		ret = cpu_op_addv((intptr_t *)&rcu_cpus.p[cpu].count[period].lock,
				1, cpu);
		if (!ret)
			break;
		assert(ret >= 0 || errno == EAGAIN);
	}
	smp_mb_slave();
	return;

norseq_fallback:
	uatomic_inc(&rcu_cpus.p[urcu_percpu_current_cpu()].count[period].lock);
	smp_mb_slave();
}

static inline void _rcu_inc_unlock(unsigned int period)
{
	int cpu, ret;

	cmm_barrier();
retry:
	/* rseq fast-path. */
	cpu = rseq_cpu_start();
	ret = rseq_addv((intptr_t *)&rcu_cpus.p[cpu].count[period].unlock,
			1, cpu);
	rseq_prepare_unload();
	if (caa_likely(!ret)) {
		cmm_barrier();
		return;
	}
	smp_mb_slave();
	/* rseq has either been aborted, or is not initialized. */
	cpu = rseq_current_cpu_raw();
	if (cpu < 0) {
		if (cpu == -1 && !urcu_rseq_register_current_thread())
			goto retry;
		goto norseq_fallback;
	}
	/* rseq has been aborted, use cpu_opv. */
	for (;;) {
		cpu = urcu_percpu_current_cpu();
		ret = cpu_op_addv((intptr_t *)&rcu_cpus.p[cpu].count[period].unlock,
				1, cpu);
		if (!ret)
			break;
		assert(ret >= 0 || errno == EAGAIN);
	}
	smp_mb_slave();
	return;

norseq_fallback:
	smp_mb_slave();
	uatomic_inc(&rcu_cpus.p[urcu_percpu_current_cpu()].count[period].unlock);
	smp_mb_slave();
}

/*
 * Helper for _rcu_read_lock().  The format of rcu_gp.ctr (as well as
 * the per-thread rcu_reader.ctr) has the upper bits containing a count of
 * _rcu_read_lock() nesting, and a lower-order bit that contains either zero
 * or RCU_GP_CTR_PHASE.  The smp_mb_slave() ensures that the accesses in
 * _rcu_read_lock() happen before the subsequent read-side critical section.
 */
static inline int _srcu_read_lock_update(void)
{
	int tmp = _CMM_LOAD_SHARED(rcu_gp.ctr);

	_rcu_inc_lock(tmp);
	return tmp;
}

/*
 * Enter an RCU read-side critical section.
 *
 * The first cmm_barrier() call ensures that the compiler does not reorder
 * the body of _rcu_read_lock() with a mutex.
 *
 * This function and its helper are both less than 10 lines long.  The
 * intent is that this function meets the 10-line criterion in LGPL,
 * allowing this function to be invoked directly from non-LGPL code.
 */
static inline int _srcu_read_lock(void)
{
	cmm_barrier();
	return _srcu_read_lock_update();
}

/*
 * This is a helper function for _rcu_read_unlock().
 *
 * The first smp_mb_slave() call ensures that the critical section is
 * seen to precede the store to rcu_reader.ctr.
 * The second smp_mb_slave() call ensures that we write to rcu_reader.ctr
 * before reading the update-side futex.
 */
static inline void _rcu_read_unlock_update_and_wakeup(int tmp)
{
	_rcu_inc_unlock(tmp);
	wake_up_gp();
}

/*
 * Exit an RCU read-side crtical section.  Both this function and its
 * helper are smaller than 10 lines of code, and are intended to be
 * usable by non-LGPL code, as called out in LGPL.
 */
static inline void _srcu_read_unlock(int period)
{
	_rcu_read_unlock_update_and_wakeup(period);
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
}

static inline void _rcu_read_lock(void)
{
	srcu_state = _srcu_read_lock();
}

static inline void _rcu_read_unlock(void)
{
	_srcu_read_unlock(srcu_state);
}

static inline int _rcu_read_ongoing(void)
{
	return 0;	//XXX TODO
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_PERCPU_STATIC_H */
