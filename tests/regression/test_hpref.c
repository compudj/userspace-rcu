// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#define _LGPL_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <stdbool.h>

#include <urcu/hpref.h>

struct test {
	int a;
	struct hpref_node node;
};

static
void test_release(struct hpref_node *node)
{
	free(caa_container_of(node, struct test, node));
}

static struct hpref_node *test_ptr;

int main(void)
{
	struct test *test;
	struct hpref_node *node;

	test = malloc(sizeof(struct test));
	if (!test)
		abort();
	test->a = 42;

	hpref_node_init(&test->node, test_release);
	/* Publish hpref pointer. */
	hpref_set_pointer(&test_ptr, &test->node);

	/* Short reader */
	node = hpref_hp_refcount_inc(&test_ptr);
	if (node) {
		printf("value: %d\n",
			caa_container_of(node, struct test, node)->a);
		poll(NULL, 0, 10);		/* sleep 10ms */
		hpref_refcount_dec(node);
	} else {
		abort();
	}

	/* Long reader. */
	node = hpref_hp_refcount_inc(&test_ptr);
	if (node) {
		sleep(1);			/* sleep 1s */
		hpref_refcount_dec(node);
	} else {
		abort();
	}

	/* Unpublish hpref pointer. */
	hpref_set_pointer(&test_ptr, NULL);	/* Store A */
	/* Wait for HP to be unused, release owner reference. */
	hpref_synchronize(&test->node);
	hpref_refcount_dec(&test->node);

	/* Not present anymore. */
	if (hpref_hp_refcount_inc(&test_ptr))
		abort();

	return 0;
}
