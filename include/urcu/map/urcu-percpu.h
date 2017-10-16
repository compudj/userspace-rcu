#ifndef _URCU_PERCPU_MAP_H
#define _URCU_PERCPU_MAP_H

/*
 * urcu-percpu-map.h
 *
 * Userspace RCU percpu header -- name mapping to allow multiple flavors
 * to be used in the same executable.
 *
 * Copyright (c) 2009, 2016 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
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

/* Mapping macros to allow multiple flavors in a single binary. */

#define srcu_read_lock			srcu_read_lock_percpu
#define _srcu_read_lock			_srcu_read_lock_percpu
#define srcu_read_unlock		srcu_read_unlock_percpu
#define _srcu_read_unlock		_srcu_read_unlock_percpu
#define rcu_read_lock			rcu_read_lock_percpu
#define _rcu_read_lock			_rcu_read_lock_percpu
#define rcu_read_unlock			rcu_read_unlock_percpu
#define _rcu_read_unlock		_rcu_read_unlock_percpu
#define rcu_register_thread		rcu_register_thread_percpu
#define rcu_unregister_thread		rcu_unregister_thread_percpu
#define rcu_init			rcu_init_percpu
#define rcu_exit			rcu_exit_percpu
#define synchronize_rcu			synchronize_rcu_percpu
#define rcu_reader			rcu_reader_percpu
#define rcu_gp				rcu_gp_percpu

#define get_cpu_call_rcu_data		get_cpu_call_rcu_data_percpu
#define get_call_rcu_thread		get_call_rcu_thread_percpu
#define create_call_rcu_data		create_call_rcu_data_percpu
#define set_cpu_call_rcu_data		set_cpu_call_rcu_data_percpu
#define get_default_call_rcu_data	get_default_call_rcu_data_percpu
#define get_call_rcu_data		get_call_rcu_data_percpu
#define get_thread_call_rcu_data	get_thread_call_rcu_data_percpu
#define set_thread_call_rcu_data	set_thread_call_rcu_data_percpu
#define create_all_cpu_call_rcu_data	create_all_cpu_call_rcu_data_percpu
#define free_all_cpu_call_rcu_data	free_all_cpu_call_rcu_data_percpu
#define call_rcu			call_rcu_percpu
#define call_rcu_data_free		call_rcu_data_free_percpu
#define call_rcu_before_fork		call_rcu_before_fork_percpu
#define call_rcu_after_fork_parent	call_rcu_after_fork_parent_percpu
#define call_rcu_after_fork_child	call_rcu_after_fork_child_percpu
#define rcu_barrier			rcu_barrier_percpu

#define defer_rcu			defer_rcu_percpu
#define rcu_defer_register_thread	rcu_defer_register_thread_percpu
#define rcu_defer_unregister_thread	rcu_defer_unregister_thread_percpu
#define rcu_defer_barrier		rcu_defer_barrier_percpu
#define rcu_defer_barrier_thread	rcu_defer_barrier_thread_percpu
#define rcu_defer_exit			rcu_defer_exit_percpu

#define rcu_flavor			rcu_flavor_percpu

#define rcu_has_sys_membarrier		rcu_has_sys_membarrier_percpu

#endif /* _URCU_PERCPU_MAP_H */
