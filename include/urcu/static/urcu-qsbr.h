#ifndef _URCU_QSBR_STATIC_H
#define _URCU_QSBR_STATIC_H

/*
 * urcu-qsbr-static.h
 *
 * Userspace RCU QSBR header.
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
#include <limits.h>
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

/*
 * This code section can only be included in LGPL 2.1 compatible source code.
 * See below for the function call wrappers which can be used in code meant to
 * be only linked with the Userspace RCU library. This comes with a small
 * performance degradation on the read-side due to the added function calls.
 * This is required to permit relinking with newer versions of the library.
 */

enum rcu_state {
	RCU_READER_ACTIVE_CURRENT,
	RCU_READER_ACTIVE_OLD,
	RCU_READER_INACTIVE,
};

#define RCU_GP_ONLINE		(1UL << 0)
#define RCU_GP_CTR		(1UL << 1)

struct rcu_gp {
	/*
	 * Global quiescent period counter with low-order bits unused.
	 * Using a int rather than a char to eliminate false register
	 * dependencies causing stalls on some architectures.
	 */
	unsigned long ctr;

	int32_t futex;
} __attribute__((aligned(CAA_CACHE_LINE_SIZE)));

extern struct rcu_gp rcu_gp;

struct rcu_reader {
	/* Data used by both reader and synchronize_rcu() */
	unsigned long ctr;
	/* Data used for registry */
	struct cds_list_head node __attribute__((aligned(CAA_CACHE_LINE_SIZE)));
	int waiting;
	pthread_t tid;
	struct rcu_gp *gp;
	/* Reader registered flag, for internal checks. */
	unsigned int registered:1;
};

struct urcu_domain;

extern DECLARE_URCU_TLS(struct rcu_reader, rcu_reader);

/*
 * Wake-up waiting synchronize_rcu(). Called from many concurrent threads.
 */
static inline void wake_up_gp(struct rcu_gp *gp, struct rcu_reader *reader_tls)
{
	if (caa_unlikely(_CMM_LOAD_SHARED(reader_tls->waiting))) {
		_CMM_STORE_SHARED(reader_tls->waiting, 0);
		cmm_smp_mb();
		if (uatomic_read(&gp->futex) != -1)
			return;
		uatomic_set(&gp->futex, 0);
		/*
		 * Ignoring return value until we can make this function
		 * return something (because urcu_die() is not publicly
		 * exposed).
		 */
		(void) futex_noasync(&gp->futex, FUTEX_WAKE, 1,
				NULL, NULL, 0);
	}
}

static inline enum rcu_state rcu_reader_state(struct rcu_gp *gp,
		struct rcu_reader *tls)
{
	unsigned long v;

	v = CMM_LOAD_SHARED(tls->ctr);
	if (!v)
		return RCU_READER_INACTIVE;
	if (v == gp->ctr)
		return RCU_READER_ACTIVE_CURRENT;
	return RCU_READER_ACTIVE_OLD;
}

/*
 * Enter an RCU read-side critical section.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _srcu_read_lock(struct urcu_domain *urcu_domain,
		struct rcu_reader *tls)
{
	urcu_assert(tls->ctr);
}

static inline void _rcu_read_lock(void)
{
	_srcu_read_lock(NULL, &URCU_TLS(rcu_reader));
}

/*
 * Exit an RCU read-side critical section.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _srcu_read_unlock(struct urcu_domain *urcu_domain,
		struct rcu_reader *tls)
{
	urcu_assert(tls->ctr);
}

static inline void _rcu_read_unlock(void)
{
	_srcu_read_unlock(NULL, &URCU_TLS(rcu_reader));
}

/*
 * Returns whether within a RCU read-side critical section.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline int _srcu_read_ongoing(struct urcu_domain *urcu_domain,
		struct rcu_reader *tls)
{
	return tls->ctr;
}

static inline int _rcu_read_ongoing(void)
{
	return _srcu_read_ongoing(NULL, &URCU_TLS(rcu_reader));
}

/*
 * This is a helper function for _rcu_quiescent_state().
 * The first cmm_smp_mb() ensures memory accesses in the prior read-side
 * critical sections are not reordered with store to
 * URCU_TLS(rcu_reader).ctr, and ensures that mutexes held within an
 * offline section that would happen to end with this
 * rcu_quiescent_state() call are not reordered with
 * store to URCU_TLS(rcu_reader).ctr.
 */
static inline void _srcu_quiescent_state_update_and_wakeup(struct rcu_gp *gp,
		struct rcu_reader *reader_tls, unsigned long gp_ctr)
{
	cmm_smp_mb();
	_CMM_STORE_SHARED(reader_tls->ctr, gp_ctr);
	cmm_smp_mb();	/* write URCU_TLS(rcu_reader).ctr before read futex */
	wake_up_gp(gp, reader_tls);
	cmm_smp_mb();
}

/*
 * Inform RCU of a quiescent state.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 *
 * We skip the memory barriers and gp store if our local ctr already
 * matches the global rcu_gp.ctr value: this is OK because a prior
 * _rcu_quiescent_state() or _rcu_thread_online() already updated it
 * within our thread, so we have no quiescent state to report.
 */
static inline void _srcu_quiescent_state(struct urcu_domain *urcu_domain,
		struct rcu_reader *reader_tls)
{
	unsigned long gp_ctr;
	struct rcu_gp *gp = reader_tls->gp;

	urcu_assert(reader_tls->registered);
	if ((gp_ctr = CMM_LOAD_SHARED(gp->ctr)) == reader_tls->ctr)
		return;
	_srcu_quiescent_state_update_and_wakeup(gp, reader_tls, gp_ctr);
}

static inline void _rcu_quiescent_state(void)
{
	_srcu_quiescent_state(NULL, &URCU_TLS(rcu_reader));
}

/*
 * Take a thread offline, prohibiting it from entering further RCU
 * read-side critical sections.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _srcu_thread_offline(struct urcu_domain *urcu_domain,
		struct rcu_reader *reader_tls)
{
	urcu_assert(reader_tls->registered);
	cmm_smp_mb();
	CMM_STORE_SHARED(reader_tls->ctr, 0);
	cmm_smp_mb();	/* write reader_tls->ctr before read futex */
	wake_up_gp(reader_tls->gp, reader_tls);
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
}

static inline void _rcu_thread_offline(void)
{
	_srcu_thread_offline(NULL, &URCU_TLS(rcu_reader));
}

/*
 * Bring a thread online, allowing it to once again enter RCU
 * read-side critical sections.
 *
 * This function is less than 10 lines long.  The intent is that this
 * function meets the 10-line criterion for LGPL, allowing this function
 * to be invoked directly from non-LGPL code.
 */
static inline void _srcu_thread_online(struct urcu_domain *urcu_domain,
		struct rcu_reader *reader_tls)
{
	urcu_assert(reader_tls->registered);
	cmm_barrier();	/* Ensure the compiler does not reorder us with mutex */
	_CMM_STORE_SHARED(reader_tls->ctr,
			CMM_LOAD_SHARED(reader_tls->gp->ctr));
	cmm_smp_mb();
}

static inline void _rcu_thread_online(void)
{
	_srcu_thread_online(NULL, &URCU_TLS(rcu_reader));
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_QSBR_STATIC_H */
