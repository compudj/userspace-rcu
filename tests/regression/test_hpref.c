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
	struct hpref_ctx ctx;

	test = malloc(sizeof(struct test));
	if (!test)
		abort();
	test->a = 42;

	hpref_node_init(&test->node, test_release);
	/* Publish hpref pointer. */
	hpref_set_pointer(&test_ptr, &test->node);

	/* Short reader */
	if (hpref_hp_get(&test_ptr, &ctx)) {
		printf("value: %d\n",
			caa_container_of(hpref_ctx_pointer(&ctx), struct test, node)->a);
		poll(NULL, 0, 10);		/* sleep 10ms */
		hpref_put(&ctx);
	} else {
		abort();
	}

	/* Long reader. */
	if (hpref_hp_get(&test_ptr, &ctx)) {
		hpref_promote_hp_to_ref(&ctx);
		sleep(1);			/* sleep 1s */
		hpref_put(&ctx);
	} else {
		abort();
	}

	/* Unpublish hpref pointer. */
	hpref_set_pointer(&test_ptr, NULL);	/* Store A */
	/* Wait for HP to be unused, release owner reference. */
	hpref_synchronize_put(&test->node);

	/* Not present anymore. */
	if (hpref_hp_get(&test_ptr, &ctx))
		abort();

	return 0;
}
