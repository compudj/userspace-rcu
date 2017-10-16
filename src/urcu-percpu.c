/*
 * urcu-percpu.c
 *
 * Userspace RCU library, percpu implementation
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

#define _BSD_SOURCE
#define _LGPL_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

#include "urcu/arch.h"
#include "urcu/wfcqueue.h"
#include "urcu/map/urcu-percpu.h"
#include "urcu/static/urcu-percpu.h"
#include "urcu-pointer.h"
#include "urcu/tls-compat.h"

#include "urcu-die.h"
#include "urcu-wait.h"

/* Do not #define _LGPL_SOURCE to ensure we can emit the wrapper symbols */
#undef _LGPL_SOURCE
#include "urcu-percpu.h"
#define _LGPL_SOURCE

__thread int srcu_state;

/*
 * If a reader is really non-cooperative and refuses to commit its
 * rcu_active_readers count to memory (there is no barrier in the reader
 * per-se), kick it after 10 loops waiting for it.
 */
#define KICK_READER_LOOPS 	10

/*
 * Active attempts to check for reader Q.S. before calling futex().
 */
#define RCU_QS_ACTIVE_ATTEMPTS 100

/* If the headers do not support membarrier system call, fall back on RCU_MB */
#ifdef __NR_membarrier
# define membarrier(...)		syscall(__NR_membarrier, __VA_ARGS__)
#else
# define membarrier(...)		-ENOSYS
#endif

int __num_possible_cpus;

static void _get_num_possible_cpus(void)
{
	int result;

	/* On Linux, when some processors are offline
	 * _SC_NPROCESSORS_CONF counts the offline
	 * processors, whereas _SC_NPROCESSORS_ONLN
	 * does not. If we used _SC_NPROCESSORS_ONLN,
	 * getcpu() could return a value greater than
	 * this sysconf, in which case the arrays
	 * indexed by processor would overflow.
	 */
	result = sysconf(_SC_NPROCESSORS_CONF);
	if (result == -1)
		return;
	__num_possible_cpus = result;
}

static inline
int num_possible_cpus(void)
{
	if (!__num_possible_cpus)
		_get_num_possible_cpus();
	return __num_possible_cpus;
}

#define for_each_possible_cpu(cpu)		\
	for ((cpu) = 0; (cpu) < num_possible_cpus(); (cpu)++)

enum membarrier_cmd {
	MEMBARRIER_CMD_QUERY = 0,
	MEMBARRIER_CMD_SHARED = (1 << 0),
};

static int init_done;
int rcu_has_sys_membarrier;

void __attribute__((constructor)) rcu_init(void);
void __attribute__((destructor)) rcu_destroy(void);

/*
 * rcu_gp_lock ensures mutual exclusion between threads calling
 * synchronize_rcu().
 */
static pthread_mutex_t rcu_gp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rcu_registry_lock = PTHREAD_MUTEX_INITIALIZER;
struct rcu_gp rcu_gp = { .ctr = 0 };

/*
 * Queue keeping threads awaiting to wait for a grace period. Contains
 * struct gp_waiters_thread objects.
 */
static DEFINE_URCU_WAIT_QUEUE(gp_waiters);

static void mutex_lock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_lock(mutex);
	if (ret)
		urcu_die(ret);
}

static void mutex_unlock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(mutex);
	if (ret)
		urcu_die(ret);
}

static void smp_mb_master(void)
{
	if (caa_likely(rcu_has_sys_membarrier))
		(void) membarrier(MEMBARRIER_CMD_SHARED, 0);
	else
		cmm_smp_mb();
}

/*
 * synchronize_rcu() waiting. Single thread.
 * Always called with rcu_registry lock held. Releases this lock and
 * grabs it again. Holds the lock when it returns.
 */
static void wait_gp(void)
{
	/*
	 * Read reader_gp before read futex.
	 */
	smp_mb_master();
	/* Temporarily unlock the registry lock. */
	mutex_unlock(&rcu_registry_lock);
	if (uatomic_read(&rcu_gp.futex) != -1)
		goto end;
	while (futex_async(&rcu_gp.futex, FUTEX_WAIT, -1,
			NULL, NULL, 0)) {
		switch (errno) {
		case EWOULDBLOCK:
			/* Value already changed. */
			goto end;
		case EINTR:
			/* Retry if interrupted by signal. */
			break;	/* Get out of switch. */
		default:
			/* Unexpected error. */
			urcu_die(errno);
		}
	}
end:
	/*
	 * Re-lock the registry lock before the next loop.
	 */
	mutex_lock(&rcu_registry_lock);
}

/*
 * Always called with rcu_registry lock held. Releases this lock between
 * iterations and grabs it again. Holds the lock when it returns.
 */
static void wait_for_cpus(void)
{
	unsigned int wait_loops = 0;
	int prev_period = 1 - !!(rcu_gp.ctr & RCU_GP_CTR_PHASE);

	/*
	 * Wait for sum of CPU lock/unlock counts to match for the
	 * previous period.
	 */
	for (;;) {
		uintptr_t sum = 0;	/* sum lock - sum unlock */
		int i;

		if (wait_loops < RCU_QS_ACTIVE_ATTEMPTS)
			wait_loops++;
		if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
			uatomic_dec(&rcu_gp.futex);
			/* Write futex before read reader_gp */
			smp_mb_master();
		}

		for_each_possible_cpu(i) {
			struct rcu_percpu *pcpu = &rcu_cpus.p[i];

			sum -= CMM_LOAD_SHARED(pcpu->count[prev_period].rseq_unlock);
			sum -= CMM_LOAD_SHARED(pcpu->count[prev_period].unlock);
		}
		/*
		 * Read unlock counts before lock counts. Reading unlock
		 * before lock count ensures we never see an unlock
		 * without having seen its associated lock, in case of a
		 * thread migration during the traversal over each cpu.
		 */
		smp_mb_master();
		for_each_possible_cpu(i) {
			struct rcu_percpu *pcpu = &rcu_cpus.p[i];

			sum += CMM_LOAD_SHARED(pcpu->count[prev_period].rseq_lock);
			sum += CMM_LOAD_SHARED(pcpu->count[prev_period].lock);
		}

		if (!sum) {
			if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
				/* Read reader_gp before write futex */
				smp_mb_master();
				uatomic_set(&rcu_gp.futex, 0);
			}
			break;
		} else {
			if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
				/* wait_gp unlocks/locks registry lock. */
				wait_gp();
			} else {
				/* Temporarily unlock the registry lock. */
				mutex_unlock(&rcu_registry_lock);
				caa_cpu_relax();
				/*
				 * Re-lock the registry lock before the
				 * next loop.
				 */
				mutex_lock(&rcu_registry_lock);
			}
		}
	}
}

void synchronize_rcu(void)
{
	DEFINE_URCU_WAIT_NODE(wait, URCU_WAIT_WAITING);
	struct urcu_waiters waiters;

	/*
	 * Add ourself to gp_waiters queue of threads awaiting to wait
	 * for a grace period. Proceed to perform the grace period only
	 * if we are the first thread added into the queue.
	 * The implicit memory barrier before urcu_wait_add()
	 * orders prior memory accesses of threads put into the wait
	 * queue before their insertion into the wait queue.
	 */
	if (urcu_wait_add(&gp_waiters, &wait) != 0) {
		/* Not first in queue: will be awakened by another thread. */
		urcu_adaptative_busy_wait(&wait);
		/* Order following memory accesses after grace period. */
		cmm_smp_mb();
		return;
	}
	/* We won't need to wake ourself up */
	urcu_wait_set_state(&wait, URCU_WAIT_RUNNING);

	mutex_lock(&rcu_gp_lock);

	/*
	 * Move all waiters into our local queue.
	 */
	urcu_move_waiters(&waiters, &gp_waiters);

	mutex_lock(&rcu_registry_lock);

	/*
	 * All threads should read qparity before accessing data structure
	 * where new ptr points to. Must be done within rcu_registry_lock
	 * because it iterates on reader threads.
	 */
	/* Write new ptr before changing the qparity */
	smp_mb_master();

	/*
	 * Wait for cpus to observe original parity or be quiescent.
	 * wait_for_cpus() can release and grab again rcu_registry_lock
	 * interally.
	 */
	wait_for_cpus();

	/*
	 * Must finish waiting for quiescent state for original parity before
	 * committing next rcu_gp.ctr update to memory. Failure to do so could
	 * result in the writer waiting forever while new readers are always
	 * accessing data (no progress).  Enforce compiler-order of load
	 * URCU_TLS(rcu_reader).ctr before store to rcu_gp.ctr.
	 */
	cmm_barrier();

	/*
	 * Adding a cmm_smp_mb() which is _not_ formally required, but makes the
	 * model easier to understand. It does not have a big performance impact
	 * anyway, given this is the write-side.
	 */
	cmm_smp_mb();

	/* Switch parity: 0 -> 1, 1 -> 0 */
	CMM_STORE_SHARED(rcu_gp.ctr, rcu_gp.ctr ^ RCU_GP_CTR_PHASE);

	/*
	 * Must commit rcu_gp.ctr update to memory before waiting for quiescent
	 * state. Failure to do so could result in the writer waiting forever
	 * while new readers are always accessing data (no progress). Enforce
	 * compiler-order of store to rcu_gp.ctr before load rcu_reader ctr.
	 */
	cmm_barrier();

	/*
	 *
	 * Adding a cmm_smp_mb() which is _not_ formally required, but makes the
	 * model easier to understand. It does not have a big performance impact
	 * anyway, given this is the write-side.
	 */
	cmm_smp_mb();

	/*
	 * Wait for cpus to observe new parity or be quiescent.
	 * wait_for_cpus() can release and grab again rcu_registry_lock
	 * interally.
	 */
	wait_for_cpus();

	/*
	 * Finish waiting for reader threads before letting the old ptr
	 * being freed. Must be done within rcu_registry_lock because it
	 * iterates on reader threads.
	 */
	smp_mb_master();

	mutex_unlock(&rcu_registry_lock);
	mutex_unlock(&rcu_gp_lock);

	/*
	 * Wakeup waiters only after we have completed the grace period
	 * and have ensured the memory barriers at the end of the grace
	 * period have been issued.
	 */
	urcu_wake_all_waiters(&waiters);
}

/*
 * library wrappers to be used by non-LGPL compatible source code.
 */

int srcu_read_lock(void)
{
	return _srcu_read_lock();
}

void srcu_read_unlock(int period)
{
	_srcu_read_unlock(period);
}

void rcu_read_lock(void)
{
	_rcu_read_lock();
}

void rcu_read_unlock(void)
{
	_rcu_read_unlock();
}

static int rcu_read_ongoing(void)
{
	return _rcu_read_ongoing();
}

static void rcu_percpu_init(void)
{
	int nr_cpus = num_possible_cpus();

	if (nr_cpus <= 0 || rcu_cpus.p)
		abort();
	rcu_cpus.p = calloc(nr_cpus, sizeof(*rcu_cpus.p));
	if (!rcu_cpus.p)
		abort();
}

void rcu_init(void)
{
	int ret;

	if (init_done)
		return;
	init_done = 1;
	ret = membarrier(MEMBARRIER_CMD_QUERY, 0);
	if (ret >= 0 && (ret & MEMBARRIER_CMD_SHARED)) {
		rcu_has_sys_membarrier = 1;
	}
	mutex_lock(&rcu_registry_lock);
	rcu_percpu_init();
	mutex_unlock(&rcu_registry_lock);
}

void rcu_destroy(void)
{
	free(rcu_cpus.p);
	rcu_cpus.p = NULL;
	init_done = 0;
}

void rcu_percpu_before_fork(void)
{
}

void rcu_percpu_after_fork_parent(void)
{
}

void rcu_percpu_after_fork_child(void)
{
	rcu_destroy();
	rcu_init();
}

void *rcu_dereference_sym_percpu(void *p)
{
	return _rcu_dereference(p);
}

void *rcu_set_pointer_sym_percpu(void **p, void *v)
{
	cmm_wmb();
	uatomic_set(p, v);
	return v;
}

void *rcu_xchg_pointer_sym_percpu(void **p, void *v)
{
	cmm_wmb();
	return uatomic_xchg(p, v);
}

void *rcu_cmpxchg_pointer_sym_percpu(void **p, void *old, void *_new)
{
	cmm_wmb();
	return uatomic_cmpxchg(p, old, _new);
}

DEFINE_RCU_FLAVOR(rcu_flavor);

#include "urcu-call-rcu-impl.h"
#include "urcu-defer-impl.h"
