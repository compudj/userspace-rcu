// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _URCU_SMARTPTR_H
#define _URCU_SMARTPTR_H

/*
 * URCU SMARTPTR: Hazard-pointer and reference count protected smart
 * pointers.
 */

#include <stdlib.h>
#include <unistd.h>
#include <poll.h>

#include <urcu/ref.h>
#include <urcu/uatomic.h>
#include <urcu/compiler.h>
#include <urcu/pointer.h>
#include <urcu/hpref.h>

/* Node within objects. */
struct urcu_smartptr_node {
	/*
	 * The hpref refcount counts the number of smart pointer
	 * references to this node.
	 */
	struct hpref_node hpref;
};

/* Reference to objects. */
struct urcu_smartptr {
	struct urcu_smartptr_node *ref;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize smartptr node and smartptr, holding an initial reference
 * to node in sptr.
 */
static inline
void urcu_smartptr_init(struct urcu_smartptr *sptr,
		struct urcu_smartptr_node *node,
		void (*smartptr_release)(struct urcu_smartptr *sptr))
{
	// node ref = 1
	// sptr initialized to ref node.
}

/* Dereference sptr_p and allocate a hazard pointer for it. */
static inline
struct urcu_smartptr_hp_ctx urcu_smartptr_hp_dereference_allocate(struct urcu_smartptr * const *sptr_p)
{
}

/* Retire hazard pointer. */
static inline
void urcu_smartptr_hp_retire(struct urcu_smartptr_ctx ctx)
{
}

/* Get the smart pointer protected by the HP context. */
static inline
struct urcu_smartptr *urcu_smartptr_hp_ctx_to_ptr(struct urcu_smartptr_hp_ctx ctx)
{
}

/* Use on a stable sptr. */
static inline
struct urcu_smartptr *urcu_smartptr_copy(struct urcu_smartptr *sptr)
{
	//ref node
	//return sptr
}

/* Use on a sptr_p which can be concurrently cleared. */
static inline
struct urcu_smartptr *urcu_smartptr_hp_dereference_copy(struct urcu_smartptr * const *sptr_p)
{
	//Use HP internally to deref sptr
	//Use urcu_smartptr_copy internally.
}

static inline
void urcu_smartptr_clear(struct urcu_smartptr **sptr)
{
	// clear ptr
	// unref node, if reach 0: release
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_SMARTPTR_H */
