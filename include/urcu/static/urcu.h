#ifndef _URCU_STATIC_H
#define _URCU_STATIC_H

/*
 * urcu-static.h
 *
 * Userspace RCU header.
 *
 * TO BE INCLUDED ONLY IN CODE THAT IS TO BE RECOMPILED ON EACH LIBURCU
 * RELEASE. See urcu.h for linking dynamically with the userspace rcu library.
 *
 * Copyright (c) 2009 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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

#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <urcu/system.h>
#include <urcu/uatomic.h>
#include <urcu/list.h>
#include <urcu/futex.h>
#include <urcu/tls-compat.h>
#include <urcu/debug.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default is RCU_MEMBARRIER */
#if !defined(RCU_MEMBARRIER) && !defined(RCU_MB) && !defined(RCU_SIGNAL)
#define RCU_MEMBARRIER
#endif

/*
 * This code section can only be included in LGPL 2.1 compatible source code.
 * See below for the function call wrappers which can be used in code meant to
 * be only linked with the Userspace RCU library. This comes with a small
 * performance degradation on the read-side due to the added function calls.
 * This is required to permit relinking with newer versions of the library.
 */

/*
 * The signal number used by the RCU library can be overridden with
 * -DSIGRCU= when compiling the library.
 * Provide backward compatibility for liburcu 0.3.x SIGURCU.
 */
#ifdef SIGURCU
#define SIGRCU SIGURCU
#endif

#ifndef SIGRCU
#define SIGRCU SIGUSR1
#endif

enum rcu_state {
	RCU_READER_ACTIVE_CURRENT,
	RCU_READER_ACTIVE_OLD,
	RCU_READER_INACTIVE,
};

/*
 * Slave barriers are only guaranteed to be ordered wrt master barriers.
 *
 * The pair ordering is detailed as (O: ordered, X: not ordered) :
 *               slave  master
 *        slave    X      O
 *        master   O      O
 */

#ifdef RCU_MEMBARRIER
extern int rcu_has_sys_membarrier;

static inline void smp_mb_slave(void)
{
	if (caa_likely(rcu_has_sys_membarrier))
		cmm_barrier();
	else
		cmm_smp_mb();
}
#endif

#ifdef RCU_MB
static inline void smp_mb_slave(void)
{
	cmm_smp_mb();
}
#endif

#ifdef RCU_SIGNAL
static inline void smp_mb_slave(void)
{
	cmm_barrier();
}
#endif

/*
 * The trick here is that RCU_GP_CTR_PHASE must be a multiple of 8 so we can use
 * a full 8-bits, 16-bits or 32-bits bitmask for the lower order bits.
 */
#define RCU_GP_COUNT		(1UL << 0)
/* Use the amount of bits equal to half of the architecture long size */
#define RCU_GP_CTR_PHASE	(1UL << (sizeof(unsigned long) << 2))
#define RCU_GP_CTR_NEST_MASK	(RCU_GP_CTR_PHASE - 1)

struct rcu_gp {
	/*
	 * Global grace period counter.
	 * Contains the current RCU_GP_CTR_PHASE.
	 * Also has a RCU_GP_COUNT of 1, to accelerate the reader fast path.
	 * Written to only by writer with mutex taken.
	 * Read by both writer and readers.
	 */
	unsigned long ctr;

	int32_t futex;
} __attribute__((aligned(CAA_CACHE_LINE_SIZE)));

struct rcu_reader {
	/* Data used by both reader and synchronize_rcu() */
	unsigned long ctr;
	char *need_mb;
	/* Data used for registry */
	struct cds_list_head node __attribute__((aligned(CAA_CACHE_LINE_SIZE)));
	pthread_t tid;
	struct rcu_gp *gp;
	/* Reader registered flag, for internal checks. */
	unsigned int registered:1;
};

extern DECLARE_URCU_TLS(struct rcu_reader, rcu_reader);

struct urcu_domain {
	/*
	 * urcu_domain.gp_lock ensures mutual exclusion between threads calling
	 * synchronize_rcu().
	 */
	pthread_mutex_t gp_lock;
	/*
	 * urcu_domain.registry_lock ensures mutual exclusion between threads
	 * registering and unregistering themselves to/from the
	 * registry, and with threads reading that registry from
	 * synchronize_rcu(). However, this lock is not held all the way
	 * through the completion of awaiting for the grace period. It
	 * is sporadically released between iterations on the registry.
	 * urcu_domain.registry_lock may nest inside urcu_domain.gp_lock.
	 */
	pthread_mutex_t registry_lock;
	struct cds_list_head registry;
	struct rcu_gp gp;
};

#define URCU_DOMAIN_INIT(urcu_domain)	\
	{ \
		.gp_lock = PTHREAD_MUTEX_INITIALIZER, \
		.registry_lock = PTHREAD_MUTEX_INITIALIZER, \
		.registry = CDS_LIST_HEAD_INIT(urcu_domain.registry), \
		.gp = { .ctr = RCU_GP_COUNT }, \
	}

/*
 * Wake-up waiting synchronize_rcu(). Called from many concurrent threads.
 */
static inline void wake_up_gp(struct rcu_gp *gp)
{
	if (caa_unlikely(uatomic_read(&gp->futex) == -1)) {
		uatomic_set(&gp->futex, 0);
		/*
		 * Ignoring return value until we can make this function
		 * return something (because urcu_die() is not publicly
		 * exposed).
		 */
		(void) futex_async(&gp->futex, FUTEX_WAKE, 1,
				NULL, NULL, 0);
	}
}

static inline enum rcu_state rcu_reader_state(struct rcu_gp *gp,
		struct rcu_reader *tls)
{
	unsigned long v;

	/*
	 * Make sure both tests below are done on the same version of *value
	 * to insure consistency.
	 */
	v = CMM_LOAD_SHARED(tls->ctr);
	if (!(v & RCU_GP_CTR_NEST_MASK))
		return RCU_READER_INACTIVE;
	if (!((v ^ gp->ctr) & RCU_GP_CTR_PHASE))
		return RCU_READER_ACTIVE_CURRENT;
	return RCU_READER_ACTIVE_OLD;
}

/*
 * Helper for _rcu_read_lock().  The format of rcu_gp.ctr (as well as
 * the per-thread rcu_reader.ctr) has the upper bits containing a count of
 * _rcu_read_lock() nesting, and a lower-order bit that contains either zero
 * or RCU_GP_CTR_PHASE.  The smp_mb_slave() ensures that the accesses in
 * _rcu_read_lock() happen before the subsequent read-side critical section.
 */
static inline void _srcu_read_lock_update(struct rcu_reader *tls,
		unsigned long tmp)
{
	if (caa_likely(!(tmp & RCU_GP_CTR_NEST_MASK))) {
		struct rcu_gp *gp = tls->gp;

		_CMM_STORE_SHARED(tls->ctr, _CMM_LOAD_SHARED(gp->ctr));
		smp_mb_slave();
	} else
		_CMM_STORE_SHARED(tls->ctr, tmp + RCU_GP_COUNT);
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
static inline void _srcu_read_lock(struct rcu_reader *tls)
{
	unsigned long tmp;

	urcu_assert(tls->registered);
	cmm_barrier();
	tmp = tls->ctr;
	urcu_assert((tmp & RCU_GP_CTR_NEST_MASK) != RCU_GP_CTR_NEST_MASK);
	_srcu_read_lock_update(tls, tmp);
}

static inline void _rcu_read_lock(void)
{
	_srcu_read_lock(&URCU_TLS(rcu_reader));
}

/*
 * This is a helper function for _rcu_read_unlock().
 *
 * The first smp_mb_slave() call ensures that the critical section is
 * seen to precede the store to rcu_reader.ctr.
 * The second smp_mb_slave() call ensures that we write to rcu_reader.ctr
 * before reading the update-side futex.
 */
static inline void _srcu_read_unlock_update_and_wakeup(struct rcu_reader *tls,
		unsigned long tmp)
{
	if (caa_likely((tmp & RCU_GP_CTR_NEST_MASK) == RCU_GP_COUNT)) {
		smp_mb_slave();
		_CMM_STORE_SHARED(tls->ctr, tmp - RCU_GP_COUNT);
		smp_mb_slave();
		wake_up_gp(tls->gp);
	} else
		_CMM_STORE_SHARED(tls->ctr, tmp - RCU_GP_COUNT);
}

/*
 * Exit an RCU read-side crtical section.  Both this function and its
 * helper are smaller than 10 lines of code, and are intended to be
 * usable by non-LGPL code, as called out in LGPL.
 */
static inline void _srcu_read_unlock(struct rcu_reader *tls)
{
	unsigned long tmp;

	urcu_assert(tls->registered);
	tmp = tls->ctr;
	urcu_assert(tmp & RCU_GP_CTR_NEST_MASK);
	_srcu_read_unlock_update_and_wakeup(tls, tmp);
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
}

static inline void _rcu_read_unlock(void)
{
	_srcu_read_unlock(&URCU_TLS(rcu_reader));
}

/*
 * Returns whether within a RCU read-side critical section.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline int _srcu_read_ongoing(struct rcu_reader *tls)
{
	return tls->ctr & RCU_GP_CTR_NEST_MASK;
}

static inline int _rcu_read_ongoing(void)
{
	return _srcu_read_ongoing(&URCU_TLS(rcu_reader));
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_STATIC_H */