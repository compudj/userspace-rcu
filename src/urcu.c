/*
 * urcu.c
 *
 * Userspace RCU library
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
#include "urcu/map/urcu.h"
#include "urcu/static/urcu.h"
#include "urcu-pointer.h"
#include "urcu/tls-compat.h"

#include "urcu-die.h"
#include "urcu-wait.h"

/* Do not #define _LGPL_SOURCE to ensure we can emit the wrapper symbols */
#undef _LGPL_SOURCE
#include "urcu.h"
#define _LGPL_SOURCE

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

enum membarrier_cmd {
	MEMBARRIER_CMD_QUERY = 0,
	MEMBARRIER_CMD_SHARED = (1 << 0),
};

#ifdef RCU_MEMBARRIER
static int init_done;
int rcu_has_sys_membarrier;

void __attribute__((constructor)) rcu_init(void);
#endif

#ifdef RCU_MB
void rcu_init(void)
{
}
#endif

#ifdef RCU_SIGNAL
static int init_done;

void __attribute__((constructor)) rcu_init(void);
void __attribute__((destructor)) rcu_exit(void);
#endif

static struct urcu_domain main_domain = URCU_DOMAIN_INIT(main_domain);

/*
 * Written to only by each individual reader. Read by both the reader and the
 * writers.
 */
DEFINE_URCU_TLS(struct rcu_reader, rcu_reader);

static DEFINE_URCU_TLS(char, urcu_need_mb);

/*
 * Queue keeping threads awaiting to wait for a grace period. Contains
 * struct gp_waiters_thread objects.
 */
static DEFINE_URCU_WAIT_QUEUE(gp_waiters);

static void mutex_lock(pthread_mutex_t *mutex)
{
	int ret;

#ifndef DISTRUST_SIGNALS_EXTREME
	ret = pthread_mutex_lock(mutex);
	if (ret)
		urcu_die(ret);
#else /* #ifndef DISTRUST_SIGNALS_EXTREME */
	while ((ret = pthread_mutex_trylock(mutex)) != 0) {
		if (ret != EBUSY && ret != EINTR)
			urcu_die(ret);
		if (CMM_LOAD_SHARED(URCU_TLS(urcu_need_mb))) {
			cmm_smp_mb();
			_CMM_STORE_SHARED(URCU_TLS(urcu_need_mb), 0);
			cmm_smp_mb();
		}
		(void) poll(NULL, 0, 10);
	}
#endif /* #else #ifndef DISTRUST_SIGNALS_EXTREME */
}

static void mutex_unlock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(mutex);
	if (ret)
		urcu_die(ret);
}

#ifdef RCU_MEMBARRIER
static void smp_mb_master(struct urcu_domain *urcu_domain)
{
	if (caa_likely(rcu_has_sys_membarrier))
		(void) membarrier(MEMBARRIER_CMD_SHARED, 0);
	else
		cmm_smp_mb();
}
#endif

#ifdef RCU_MB
static void smp_mb_master(struct urcu_domain *urcu_domain)
{
	cmm_smp_mb();
}
#endif

#ifdef RCU_SIGNAL
static void force_mb_all_readers(struct urcu_domain *urcu_domain)
{
	struct rcu_reader *index;

	/*
	 * Ask for each threads to execute a cmm_smp_mb() so we can consider the
	 * compiler barriers around rcu read lock as real memory barriers.
	 */
	if (cds_list_empty(&urcu_domain->registry))
		return;
	/*
	 * pthread_kill has a cmm_smp_mb(). But beware, we assume it performs
	 * a cache flush on architectures with non-coherent cache. Let's play
	 * safe and don't assume anything : we use cmm_smp_mc() to make sure the
	 * cache flush is enforced.
	 */
	cds_list_for_each_entry(index, &urcu_domain->registry, node) {
		CMM_STORE_SHARED(*index->need_mb, 1);
		pthread_kill(index->tid, SIGRCU);
	}
	/*
	 * Wait for sighandler (and thus mb()) to execute on every thread.
	 *
	 * Note that the pthread_kill() will never be executed on systems
	 * that correctly deliver signals in a timely manner.  However, it
	 * is not uncommon for kernels to have bugs that can result in
	 * lost or unduly delayed signals.
	 *
	 * If you are seeing the below pthread_kill() executing much at
	 * all, we suggest testing the underlying kernel and filing the
	 * relevant bug report.  For Linux kernels, we recommend getting
	 * the Linux Test Project (LTP).
	 */
	cds_list_for_each_entry(index, &urcu_domain->registry, node) {
		while (CMM_LOAD_SHARED(*index->need_mb)) {
			pthread_kill(index->tid, SIGRCU);
			(void) poll(NULL, 0, 1);
		}
	}
	cmm_smp_mb();	/* read ->need_mb before ending the barrier */
}

static void smp_mb_master(struct urcu_domain *urcu_domain)
{
	force_mb_all_readers(urcu_domain);
}
#endif /* #ifdef RCU_SIGNAL */

/*
 * synchronize_rcu() waiting. Single thread.
 * Always called with rcu registry lock held. Releases this lock and
 * grabs it again. Holds the lock when it returns.
 */
static void wait_gp(struct urcu_domain *urcu_domain)
{
	/*
	 * Read reader_gp before read futex. smp_mb_master() needs to
	 * be called with the rcu registry lock held in RCU_SIGNAL
	 * flavor.
	 */
	smp_mb_master(urcu_domain);
	/* Temporarily unlock the registry lock. */
	mutex_unlock(&urcu_domain->registry_lock);
	if (uatomic_read(&urcu_domain->gp.futex) != -1)
		goto end;
	while (futex_async(&urcu_domain->gp.futex, FUTEX_WAIT, -1,
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
	mutex_lock(&urcu_domain->registry_lock);
}

/*
 * Always called with rcu_registry lock held. Releases this lock between
 * iterations and grabs it again. Holds the lock when it returns.
 */
static void wait_for_readers(struct urcu_domain *urcu_domain,
			struct cds_list_head *input_readers,
			struct cds_list_head *cur_snap_readers,
			struct cds_list_head *qsreaders)
{
	unsigned int wait_loops = 0;
	struct rcu_reader *index, *tmp;
	struct rcu_gp *gp = &urcu_domain->gp;
#ifdef HAS_INCOHERENT_CACHES
	unsigned int wait_gp_loops = 0;
#endif /* HAS_INCOHERENT_CACHES */

	/*
	 * Wait for each thread URCU_TLS(rcu_reader).ctr to either
	 * indicate quiescence (not nested), or observe the current
	 * gp.ctr value.
	 */
	for (;;) {
		if (wait_loops < RCU_QS_ACTIVE_ATTEMPTS)
			wait_loops++;
		if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
			uatomic_dec(&urcu_domain->gp.futex);
			/* Write futex before read reader_gp */
			smp_mb_master(urcu_domain);
		}

		cds_list_for_each_entry_safe(index, tmp, input_readers, node) {
			switch (rcu_reader_state(gp, index)) {
			case RCU_READER_ACTIVE_CURRENT:
				if (cur_snap_readers) {
					cds_list_move(&index->node,
						cur_snap_readers);
					break;
				}
				/* Fall-through */
			case RCU_READER_INACTIVE:
				cds_list_move(&index->node, qsreaders);
				break;
			case RCU_READER_ACTIVE_OLD:
				/*
				 * Old snapshot. Leaving node in
				 * input_readers will make us busy-loop
				 * until the snapshot becomes current or
				 * the reader becomes inactive.
				 */
				break;
			}
		}

#ifndef HAS_INCOHERENT_CACHES
		if (cds_list_empty(input_readers)) {
			if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
				/* Read reader_gp before write futex */
				smp_mb_master(urcu_domain);
				uatomic_set(&urcu_domain->gp.futex, 0);
			}
			break;
		} else {
			if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
				/* wait_gp unlocks/locks registry lock. */
				wait_gp(urcu_domain);
			} else {
				/* Temporarily unlock the registry lock. */
				mutex_unlock(&urcu_domain->registry_lock);
				caa_cpu_relax();
				/*
				 * Re-lock the registry lock before the
				 * next loop.
				 */
				mutex_lock(&urcu_domain->registry_lock);
			}
		}
#else /* #ifndef HAS_INCOHERENT_CACHES */
		/*
		 * BUSY-LOOP. Force the reader thread to commit its
		 * URCU_TLS(rcu_reader).ctr update to memory if we wait
		 * for too long.
		 */
		if (cds_list_empty(input_readers)) {
			if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
				/* Read reader_gp before write futex */
				smp_mb_master(urcu_domain);
				uatomic_set(&urcu_domain->gp.futex, 0);
			}
			break;
		} else {
			if (wait_gp_loops == KICK_READER_LOOPS) {
				smp_mb_master(urcu_domain);
				wait_gp_loops = 0;
			}
			if (wait_loops >= RCU_QS_ACTIVE_ATTEMPTS) {
				/* wait_gp unlocks/locks registry lock. */
				wait_gp(urcu_domain);
				wait_gp_loops++;
			} else {
				/* Temporarily unlock the registry lock. */
				mutex_unlock(&urcu_domain->registry_lock);
				caa_cpu_relax();
				/*
				 * Re-lock the registry lock before the
				 * next loop.
				 */
				mutex_lock(&urcu_domain->registry_lock);
			}
		}
#endif /* #else #ifndef HAS_INCOHERENT_CACHES */
	}
}

void synchronize_srcu(struct urcu_domain *urcu_domain)
{
	CDS_LIST_HEAD(cur_snap_readers);
	CDS_LIST_HEAD(qsreaders);
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

	mutex_lock(&urcu_domain->gp_lock);

	/*
	 * Move all waiters into our local queue.
	 */
	urcu_move_waiters(&waiters, &gp_waiters);

	mutex_lock(&urcu_domain->registry_lock);

	if (cds_list_empty(&urcu_domain->registry))
		goto out;

	/*
	 * All threads should read qparity before accessing data structure
	 * where new ptr points to. Must be done within
	 * urcu_domain->registry_lock because it iterates on reader
	 * threads.
	 */
	/* Write new ptr before changing the qparity */
	smp_mb_master(urcu_domain);

	/*
	 * Wait for readers to observe original parity or be quiescent.
	 * wait_for_readers() can release and grab again
	 * urcu_domain->registry_lock interally.
	 */
	wait_for_readers(urcu_domain, &urcu_domain->registry,
			&cur_snap_readers, &qsreaders);

	/*
	 * Must finish waiting for quiescent state for original parity before
	 * committing next gp.ctr update to memory. Failure to do so could
	 * result in the writer waiting forever while new readers are always
	 * accessing data (no progress).  Enforce compiler-order of load
	 * URCU_TLS(rcu_reader).ctr before store to gp.ctr.
	 */
	cmm_barrier();

	/*
	 * Adding a cmm_smp_mb() which is _not_ formally required, but makes the
	 * model easier to understand. It does not have a big performance impact
	 * anyway, given this is the write-side.
	 */
	cmm_smp_mb();

	/* Switch parity: 0 -> 1, 1 -> 0 */
	CMM_STORE_SHARED(urcu_domain->gp.ctr,
			urcu_domain->gp.ctr ^ RCU_GP_CTR_PHASE);

	/*
	 * Must commit gp.ctr update to memory before waiting for quiescent
	 * state. Failure to do so could result in the writer waiting forever
	 * while new readers are always accessing data (no progress). Enforce
	 * compiler-order of store to gp.ctr before load rcu_reader ctr.
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
	 * Wait for readers to observe new parity or be quiescent.
	 * wait_for_readers() can release and grab again
	 * urcu_domain->registry_lock interally.
	 */
	wait_for_readers(urcu_domain, &cur_snap_readers, NULL, &qsreaders);

	/*
	 * Put quiescent reader list back into registry.
	 */
	cds_list_splice(&qsreaders, &urcu_domain->registry);

	/*
	 * Finish waiting for reader threads before letting the old ptr
	 * being freed. Must be done within urcu_domain->registry_lock
	 * because it iterates on reader threads.
	 */
	smp_mb_master(urcu_domain);
out:
	mutex_unlock(&urcu_domain->registry_lock);
	mutex_unlock(&urcu_domain->gp_lock);

	/*
	 * Wakeup waiters only after we have completed the grace period
	 * and have ensured the memory barriers at the end of the grace
	 * period have been issued.
	 */
	urcu_wake_all_waiters(&waiters);
}

void synchronize_rcu(void)
{
	synchronize_srcu(&main_domain);
}

/*
 * library wrappers to be used by non-LGPL compatible source code.
 */

void srcu_read_lock(struct rcu_reader *reader_tls)
{
	_srcu_read_lock(reader_tls);
}

void srcu_read_unlock(struct rcu_reader *reader_tls)
{
	_srcu_read_unlock(reader_tls);
}

int srcu_read_ongoing(struct rcu_reader *reader_tls)
{
	return _srcu_read_ongoing(reader_tls);
}

void rcu_read_lock(void)
{
	_rcu_read_lock();
}

void rcu_read_unlock(void)
{
	_rcu_read_unlock();
}

int rcu_read_ongoing(void)
{
	return _rcu_read_ongoing();
}

struct urcu_domain *urcu_create_domain(void)
{
	struct urcu_domain *urcu_domain;

	urcu_domain = calloc(1, sizeof(*urcu_domain));
	if (!urcu_domain)
		return NULL;
	if (pthread_mutex_init(&urcu_domain->gp_lock, NULL))
		abort();
	if (pthread_mutex_init(&urcu_domain->registry_lock, NULL))
		abort();
	CDS_INIT_LIST_HEAD(&urcu_domain->registry);
	urcu_domain->gp.ctr = RCU_GP_COUNT;
	return urcu_domain;
}

void urcu_destroy_domain(struct urcu_domain *urcu_domain)
{
	if (!cds_list_empty(&urcu_domain->registry))
		abort();
	if (pthread_mutex_destroy(&urcu_domain->gp_lock))
		abort();
	if (pthread_mutex_destroy(&urcu_domain->registry_lock))
		abort();
	free(urcu_domain);
}

struct rcu_reader *urcu_create_reader_tls(void)
{
	return calloc(1, sizeof(struct rcu_reader));
}

void urcu_destroy_reader_tls(struct rcu_reader *reader_tls)
{
	free(reader_tls);
}

void srcu_register_thread(struct urcu_domain *urcu_domain,
		struct rcu_reader *reader_tls)
{
	reader_tls->tid = pthread_self();
	assert(reader_tls->need_mb == NULL);
	reader_tls->need_mb = &URCU_TLS(urcu_need_mb);
	assert(!(reader_tls->ctr & RCU_GP_CTR_NEST_MASK));

	mutex_lock(&urcu_domain->registry_lock);
	assert(!reader_tls->registered);
	reader_tls->gp = &urcu_domain->gp;
	reader_tls->registered = 1;
	rcu_init();	/* In case gcc does not support constructor attribute */
	cds_list_add(&reader_tls->node, &urcu_domain->registry);
	mutex_unlock(&urcu_domain->registry_lock);
}

void rcu_register_thread(void)
{
	srcu_register_thread(&main_domain, &URCU_TLS(rcu_reader));
}

void srcu_unregister_thread(struct urcu_domain *urcu_domain,
		struct rcu_reader *reader_tls)
{
	mutex_lock(&urcu_domain->registry_lock);
	assert(reader_tls->registered);
	reader_tls->registered = 0;
	cds_list_del(&reader_tls->node);
	reader_tls->need_mb = NULL;
	reader_tls->gp = NULL;
	mutex_unlock(&urcu_domain->registry_lock);
}

void rcu_unregister_thread(void)
{
	srcu_unregister_thread(&main_domain, &URCU_TLS(rcu_reader));
}

#ifdef RCU_MEMBARRIER
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
}
#endif

#ifdef RCU_SIGNAL
static void sigrcu_handler(int signo, siginfo_t *siginfo, void *context)
{
	/*
	 * Executing this cmm_smp_mb() is the only purpose of this signal handler.
	 * It punctually promotes cmm_barrier() into cmm_smp_mb() on every thread it is
	 * executed on.
	 */
	cmm_smp_mb();
	_CMM_STORE_SHARED(URCU_TLS(urcu_need_mb), 0);
	cmm_smp_mb();
}

/*
 * rcu_init constructor. Called when the library is linked, but also when
 * reader threads are calling rcu_register_thread().
 * Should only be called by a single thread at a given time. This is ensured by
 * holing the urcu_domain->registry_lock from rcu_register_thread() or by
 * running at library load time, which should not be executed by
 * multiple threads nor concurrently with rcu_register_thread() anyway.
 */
void rcu_init(void)
{
	struct sigaction act;
	int ret;

	if (init_done)
		return;
	init_done = 1;

	act.sa_sigaction = sigrcu_handler;
	act.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&act.sa_mask);
	ret = sigaction(SIGRCU, &act, NULL);
	if (ret)
		urcu_die(errno);
}

void rcu_exit(void)
{
	/*
	 * Don't unregister the SIGRCU signal handler anymore, because
	 * call_rcu threads could still be using it shortly before the
	 * application exits.
	 * Assertion disabled because call_rcu threads are now rcu
	 * readers, and left running at exit.
	 * assert(cds_list_empty(&urcu_domain->registry));
	 */
}

#endif /* #ifdef RCU_SIGNAL */

DEFINE_RCU_FLAVOR(rcu_flavor);

#include "urcu-call-rcu-impl.h"
#include "urcu-defer-impl.h"
