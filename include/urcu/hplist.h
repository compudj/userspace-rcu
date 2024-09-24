// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _URCU_HPLIST_H
#define _URCU_HPLIST_H

#include <urcu/rculist.h>
#include <urcu/compiler.h>

struct hplist_head {
	struct cds_list_head reader_head;
	struct cds_list_head writer_head;
};

#define HPLIST_HEAD_INIT(name) \
{ \
	.reader_head = CDS_LIST_HEAD_INIT(&(name).reader_head), \
	.writer_head = CDS_LIST_HEAD_INIT(&(name).writer_head), \
}

#define HPLIST_HEAD(name) \
	struct hplist_head name = HPLIST_HEAD_INIT(name)

static inline
void init_hplist_head(struct hplist_head *ptr)
{
	CDS_INIT_LIST_HEAD(&(ptr)->reader_head);
	CDS_INIT_LIST_HEAD(&(ptr)->writer_head);
}

/* Insert new element to head of hazard pointer protected list. */
static inline
void hplist_insert_head(struct hplist_head *newp, struct hplist_head *head)
{
	cds_list_add(&newp->writer_head, &head->writer_head);
	cds_list_add_rcu(&newp->reader_head, &head->reader_head);
}

/* Insert new element to tail of hazard pointer protected list. */
static inline
void hplist_insert_tail(struct hplist_head *newp, struct hplist_head *head)
{
	cds_list_add_tail(&newp->writer_head, &head->writer_head);
	cds_list_add_tail_rcu(&newp->reader_head, &head->reader_head);
}

/*
 * Hide element from HP protected list readers. In addition to update the
 * prev/next reader list nodes next/prev pointers, also update the prev/next
 * writer list nodes next/prev reader list pointers to make sure that
 * all reader next pointers to this element become invisible to all
 * readers, even those holding HP references to already hidden prev/next
 * nodes.
 */
static inline
void hplist_hide_from_readers(struct hplist_head *elem)
{
	struct hplist_head *iter;

	/*
	 * Iterate backwards over all hidden elements until we reach the
	 * previous currently visible element, and update their reader
	 * next pointer (currently pointing to @elem) to point to the
	 * next visible element.
	 * The reader_head prev pointers are only useful when nodes are
	 * visible. Don't bother updating reader_head prev for iteration
	 * elements which are hidden.
	 */
	for (iter = caa_container_of(elem->writer_head.prev, struct hplist_head, writer_head);
			&iter->reader_head != elem->reader_head.prev;
			iter = caa_container_of(iter->writer_head.prev, struct hplist_head, writer_head))
		CMM_STORE_SHARED(iter->reader_head.next, elem->reader_head.next);
	cds_list_del_rcu(&elem->reader_head);
}

/*
 * Remove element from writer list. The @elem hazard pointer should be
 * synchronized between hplist_hide_from_readers() and hplist_remove().
 */
static inline
void hplist_remove(struct hplist_head *elem)
{
	cds_list_del(&elem->writer_head);
}

#endif /* _URCU_HPLIST_H */
