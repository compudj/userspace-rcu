#ifndef _URCU_QSBR_H
#define _URCU_QSBR_H

/*
 * urcu-qsbr.h
 *
 * Userspace RCU QSBR header.
 *
 * LGPL-compatible code should include this header with :
 *
 * #define _LGPL_SOURCE
 * #include <urcu.h>
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

/*
 * See urcu-pointer.h and urcu/static/urcu-pointer.h for pointer
 * publication headers.
 */
#include <urcu-pointer.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <urcu/map/urcu-qsbr.h>

#ifdef RCU_DEBUG	/* For backward compatibility */
#define DEBUG_RCU
#endif

/*
 * Important !
 *
 * Each thread containing read-side critical sections must be registered
 * with rcu_register_thread() before calling rcu_read_lock().
 * rcu_unregister_thread() should be called before the thread exits.
 */

struct urcu_domain;
struct rcu_reader;

#ifdef _LGPL_SOURCE

#include <urcu/static/urcu-qsbr.h>

/*
 * Mappings for static use of the userspace RCU library.
 * Should only be used in LGPL-compatible code.
 */

/*
 * rcu_read_lock()
 * rcu_read_unlock()
 *
 * Mark the beginning and end of a read-side critical section.
 * DON'T FORGET TO USE rcu_register_thread/rcu_unregister_thread()
 * FOR EACH THREAD WITH READ-SIDE CRITICAL SECTION.
 */
#define srcu_read_lock_qsbr		_srcu_read_lock
#define srcu_read_unlock_qsbr		_srcu_read_unlock
#define srcu_read_ongoing_qsbr		_srcu_read_ongoing

#define srcu_quiescent_state_qsbr	_srcu_quiescent_state
#define srcu_thread_offline_qsbr	_srcu_thread_offline
#define srcu_thread_online_qsbr		_srcu_thread_online

#define rcu_read_lock_qsbr		_rcu_read_lock
#define rcu_read_unlock_qsbr		_rcu_read_unlock
#define rcu_read_ongoing_qsbr		_rcu_read_ongoing

#define rcu_quiescent_state_qsbr	_rcu_quiescent_state
#define rcu_thread_offline_qsbr		_rcu_thread_offline
#define rcu_thread_online_qsbr		_rcu_thread_online

#else /* !_LGPL_SOURCE */

/*
 * library wrappers to be used by non-LGPL compatible source code.
 */

/*
 * QSBR read lock/unlock are guaranteed to be no-ops. Therefore, we expose them
 * in the LGPL header for any code to use. However, the debug version is not
 * nops and may contain sanity checks. To activate it, applications must be
 * recompiled with -DDEBUG_RCU (even non-LGPL/GPL applications). This is the
 * best trade-off between license/performance/code triviality and
 * library debugging & tracing features we could come up with.
 */

#if (!defined(BUILD_QSBR_LIB) && !defined(DEBUG_RCU))

static inline void srcu_read_lock(struct rcu_reader *reader_tls)
{
}

static inline void srcu_read_unlock(struct rcu_reader *reader_tls)
{
}

static inline void rcu_read_lock(void)
{
}

static inline void rcu_read_unlock(void)
{
}

#else /* !DEBUG_RCU */

extern void srcu_read_lock(struct rcu_reader *reader_tls);
extern void srcu_read_unlock(struct rcu_reader *reader_tls);
extern void rcu_read_lock(void);
extern void rcu_read_unlock(void);

#endif /* !DEBUG_RCU */

extern int srcu_read_ongoing(struct rcu_reader *reader_tls);
extern void srcu_quiescent_state(struct rcu_reader *reader_tls);
extern void srcu_thread_offline(struct rcu_reader *reader_tls);
extern void srcu_thread_online(struct rcu_reader *reader_tls);

extern int rcu_read_ongoing(void);
extern void rcu_quiescent_state(void);
extern void rcu_thread_offline(void);
extern void rcu_thread_online(void);

#endif /* !_LGPL_SOURCE */

extern void synchronize_srcu(struct urcu_domain *urcu_domain);

extern void synchronize_rcu(void);

extern struct urcu_domain *urcu_create_domain(void);
extern void urcu_destroy_domain(struct urcu_domain *urcu_domain);

extern struct rcu_reader *urcu_create_reader_tls(void);
extern void urcu_destroy_reader_tls(struct rcu_reader *reader_tls);

/*
 * Reader thread registration.
 */
extern void srcu_register_thread(struct urcu_domain *urcu_domain,
		struct rcu_reader *reader_tls);
extern void srcu_unregister_thread(struct urcu_domain *urcu_domain,
		struct rcu_reader *reader_tls);

extern void rcu_register_thread(void);
extern void rcu_unregister_thread(void);

#ifdef __cplusplus
}
#endif

#include <urcu-call-rcu.h>
#include <urcu-defer.h>
#include <urcu-flavor.h>

#endif /* _URCU_QSBR_H */
