// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * test_false_sharing.c
 *
 * Userspace RCU library - test false sharing
 */

#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "tap.h"

struct thread_param {
	char *base;
	size_t index;
	size_t stride;
};

/* stop test. */
static int stop;

static int opt_nr_threads = 4,
	opt_test_duration = 4,		/* s */
	opt_stride = 128,
	opt_verbose = 0;

#define dbg_printf(fmt, ...)					\
do {								\
	if (opt_verbose)					\
		printf("[debug] " fmt, ## __VA_ARGS__);		\
} while (0)

static int64_t difftimespec_ns(const struct timespec after, const struct timespec before)
{
    return ((int64_t) after.tv_sec - (int64_t) before.tv_sec) * (int64_t) 1000000000
		+ ((int64_t) after.tv_nsec - (int64_t) before.tv_nsec);
}

static void *counter_thread(void *arg)
{
	struct thread_param *param = (struct thread_param *) arg;
	size_t offset = param->index * param->stride;
	char *base = param->base;
	uint64_t *counter = (uint64_t *) (base + offset);

	while (!__atomic_load_n(&stop, __ATOMIC_RELAXED))
		__atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
	return NULL;
}

static void show_usage(char **argv)
{
	printf("Usage : %s <OPTIONS>\n",
		argv[0]);
	printf("OPTIONS:\n");
	printf("	[-D N] Test duration in s (default 20)\n");
	printf("	[-t N] Number of counter threads (default 8)\n");
	printf("	[-s N] Allocation stride (default 128)\n");
	printf("	[-v] Verbose output.\n");
	printf("	[-h] Show this help.\n");
	printf("\n");
}

static int parse_args(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		int value = 0;
		char opt;

		if (argv[i][0] != '-')
			continue;
		opt = argv[i][1];
		switch (opt) {
		case 't':
		case 's':
		case 'D':
			if (argc < i + 2) {
				show_usage(argv);
				goto error;
			}
			value = atol(argv[i + 1]);
			i++;
			break;
		case 'h':
			show_usage(argv);
			goto end;
		case 'v':
			opt_verbose = 1;
			break;
		default:
			show_usage(argv);
			goto error;
		}

		switch (opt) {
		case 't':
			opt_nr_threads = value;
			break;
		case 's':
			opt_stride = value;
			break;
		case 'D':
			opt_test_duration = value;
			break;
		default:
			break;
		}
	}
	return 0;
end:
	return 1;
error:
	return -1;
}

int main(int argc, char **argv)
{
	struct thread_param *param;
	pthread_t *thread_id;
	unsigned int left;
	char *base;
	int ret, err;
	struct timespec begin, end;
	uint64_t time_delta_ns, increment_total = 0;

	ret = parse_args(argc, argv);
	if (ret < 0)
		abort();
	if (ret == 1)
		exit(0);

	plan_no_plan();

	if ((size_t) opt_stride < sizeof(uint64_t)) {
		fprintf(stderr, "Stride %d too small\n", opt_stride);
		exit(-1);
	}

	dbg_printf("Number of threads: %d\n", opt_nr_threads);
	dbg_printf("Test duration: %d seconds\n", opt_test_duration);
	dbg_printf("Stride: %d\n", opt_stride);

	thread_id = (pthread_t *) calloc(opt_nr_threads, sizeof(pthread_t));
	if (!thread_id)
		abort();
	param = (struct thread_param *) calloc(opt_nr_threads, sizeof(struct thread_param));
	if (!param)
		abort();
	base = aligned_alloc(opt_stride, opt_nr_threads * opt_stride);
	if (!base)
		abort();
	memset(base, 0, opt_nr_threads * opt_stride);

	if (clock_gettime(CLOCK_MONOTONIC, &begin))
		abort();
	for (int k = 0; k < opt_nr_threads; ++k) {
		param[k].base = base;
		param[k].index = k;
		param[k].stride = opt_stride;
		err = pthread_create(&thread_id[k], NULL, counter_thread, &param[k]);
		if (err != 0)
			exit(1);
	}

	left = opt_test_duration;
	do {
		left = sleep(left);
	} while (left);

	__atomic_store_n(&stop, 1, __ATOMIC_RELAXED);

	for (int k = 0; k < opt_nr_threads; ++k) {
		pthread_join(thread_id[k], NULL);
	}
	if (clock_gettime(CLOCK_MONOTONIC, &end))
		abort();
	time_delta_ns = difftimespec_ns(end, begin);

	dbg_printf("Test completed in: %" PRIu64 "ms\n", time_delta_ns / 1000000);

	for (int k = 0; k < opt_nr_threads; k++) {
		increment_total += *(uint64_t *) (base + (k * opt_stride));
	}
	dbg_printf("Increment total: %" PRIu64 "\n", increment_total);
	ok(1, "Stride %d bytes, increments per ms per thread: %" PRIu64, opt_stride,
		(1000000 * increment_total) / (time_delta_ns * opt_nr_threads));

	free(base);
	free(param);
	free(thread_id);

	return exit_status();
}
