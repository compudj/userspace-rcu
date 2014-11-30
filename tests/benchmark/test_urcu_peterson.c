/*
 * test_urcu_peterson.c
 *
 * Userspace RCU library - Test 2-class mutex based on Peterson
 *
 * Copyright February 2014 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include "config.h"
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include <urcu/arch.h>
#include <urcu/tls-compat.h>
#include <urcu/uatomic.h>
#include "cpuset.h"
#include "thread-id.h"

#include <urcu/peterson-mutex.h>

/* hardcoded number of CPUs */
#define NR_CPUS 16384

#ifndef DYNAMIC_LINK_TEST
#define _LGPL_SOURCE
#endif

static unsigned long duration;

/* read-side C.S. duration, in loops */
static unsigned long fast_cs_len, slow_cs_len;
static unsigned long fast_delay, slow_delay;

static inline void loop_sleep(unsigned long loops)
{
	while (loops-- != 0)
		caa_cpu_relax();
}

static int verbose_mode;

static volatile int test_stop, test_go;

#define printf_verbose(fmt, args...)		\
	do {					\
		if (verbose_mode)		\
			printf(fmt, ## args);	\
	} while (0)

static unsigned int cpu_affinities[NR_CPUS];
static unsigned int next_aff = 0;
static int use_affinity = 0;

pthread_mutex_t affinity_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_affinity(void)
{
#if HAVE_SCHED_SETAFFINITY
	cpu_set_t mask;
	int cpu, ret;
#endif /* HAVE_SCHED_SETAFFINITY */

	if (!use_affinity)
		return;

#if HAVE_SCHED_SETAFFINITY
	ret = pthread_mutex_lock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex lock");
		exit(-1);
	}
	cpu = cpu_affinities[next_aff++];
	ret = pthread_mutex_unlock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex unlock");
		exit(-1);
	}

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
#if SCHED_SETAFFINITY_ARGS == 2
	sched_setaffinity(0, &mask);
#else
	sched_setaffinity(0, sizeof(mask), &mask);
#endif
#endif /* HAVE_SCHED_SETAFFINITY */
}

/*
 * returns 0 if test should end.
 */
static int test_duration(void)
{
	return !test_stop;
}

static DEFINE_URCU_TLS(unsigned long long, nr_loops);

static unsigned int nr_fast;
static unsigned int nr_slow;

static struct urcu_peterson_mutex pm;
static __thread struct urcu_peterson_tls pt;
static __thread int is_fast;

static volatile int testval;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void do_fast(void)
{
	int readval;

	urcu_pt_mutex_lock_single_fast(&pm, &pt);
	//pthread_mutex_lock(&mutex);
	readval = testval;
	testval++;
	assert(testval == readval + 1);
	testval--;
	if (caa_unlikely(fast_cs_len))
		loop_sleep(fast_cs_len);
	//pthread_mutex_unlock(&mutex);
	urcu_pt_mutex_unlock_single_fast(&pm, &pt);
}

static void do_slow(void)
{
	int readval;

	urcu_pt_mutex_lock_multi_slow(&pm, &pt);
	readval = testval;
	testval++;
	assert(testval == readval + 1);
	testval--;
	if (caa_unlikely(slow_cs_len))
		loop_sleep(slow_cs_len);
	urcu_pt_mutex_unlock_multi_slow(&pm, &pt);
}

static void sighandler(int sig)
{
	switch (sig) {
	case SIGUSR1:
		if (is_fast)
			do_fast();
		else
			do_slow();
		break;
	default:
		break;
	}
}

static void *thr_fast(void *_count)
{
	unsigned long long *count = _count;

	is_fast = 1;

	printf_verbose("thread_begin %s, tid %lu\n",
			"fast", urcu_get_thread_id());

	set_affinity();

	while (!test_go)
	{
	}
	cmm_smp_mb();

	for (;;) {
		do_fast();

		if (caa_unlikely(fast_delay))
			loop_sleep(fast_delay);

		URCU_TLS(nr_loops)++;
		if (caa_unlikely(!test_duration()))
			break;
	}

	count[0] = URCU_TLS(nr_loops);
	printf_verbose("fast thread_end, tid %lu, "
			"loops %llu\n",
			urcu_get_thread_id(),
			URCU_TLS(nr_loops));
	return ((void*)1);
}

static void *thr_slow(void *_count)
{
	unsigned long long *count = _count;

	is_fast = 0;

	printf_verbose("thread_begin %s, tid %lu\n",
			"slow", urcu_get_thread_id());

	set_affinity();

	while (!test_go)
	{
	}
	cmm_smp_mb();

	for (;;) {
		do_slow();

		if (caa_unlikely(slow_delay))
			loop_sleep(slow_delay);

		URCU_TLS(nr_loops)++;
		if (caa_unlikely(!test_duration()))
			break;
	}

	count[0] = URCU_TLS(nr_loops);
	printf_verbose("slow thread_end, tid %lu, "
			"loops %llu\n",
			urcu_get_thread_id(),
			URCU_TLS(nr_loops));
	return ((void*)1);
}

static void show_usage(int argc, char **argv)
{
	printf("Usage : %s nr_fast nr_slow duration (s) <OPTIONS>\n",
		argv[0]);
	printf("OPTIONS:\n");
	printf("	[-f len] (fast class c.s. len (loops))\n");
	printf("	[-s len] (slow class c.s. len (loops))\n");
	printf("	[-F delay] (fast class delay loops)\n");
	printf("	[-S delay] (slow class delay loops)\n");
	printf("	[-v] (verbose output)\n");
	printf("	[-a cpu#] [-a cpu#]... (affinity)\n");
	printf("\n");
}

static int set_signal_handler(void)
{
	int ret = 0;
	struct sigaction sa;
	sigset_t sigset;

	if ((ret = sigemptyset(&sigset)) < 0) {
		perror("sigemptyset");
		return ret;
	}

	sa.sa_handler = sighandler;
	sa.sa_mask = sigset;
	sa.sa_flags = 0;
	if ((ret = sigaction(SIGUSR1, &sa, NULL)) < 0) {
		perror("sigaction");
		return ret;
	}
	printf("Signal handler set for SIGUSR1\n");

	return ret;
}

int main(int argc, char **argv)
{
	int err;
	pthread_t *tid_fast, *tid_slow;
	void *tret;
	unsigned long long *count_fast, *count_slow;
	unsigned long long tot_fast = 0, tot_slow= 0;
	unsigned long long tot_fast_loops = 0,
			   tot_slow_loops = 0,
			   tot_loops = 0;
	int i, a, retval = 0;
	struct timespec init_time;

	if (argc < 4) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[1], "%u", &nr_fast);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[2], "%u", &nr_slow);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}
	
	err = sscanf(argv[3], "%lu", &duration);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}

	for (i = 4; i < argc; i++) {
		if (argv[i][0] != '-')
			continue;
		switch (argv[i][1]) {
		case 'a':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			a = atoi(argv[++i]);
			cpu_affinities[next_aff++] = a;
			use_affinity = 1;
			printf_verbose("Adding CPU %d affinity\n", a);
			break;
		case 'F':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			fast_delay = atol(argv[++i]);
			break;
		case 'S':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			slow_delay = atol(argv[++i]);
			break;
		case 'f':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			fast_cs_len = atol(argv[++i]);
			break;
		case 's':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			slow_cs_len = atol(argv[++i]);
			break;
		case 'v':
			verbose_mode = 1;
			break;
		}
	}

	if (nr_fast > 1) {
		fprintf(stderr, "[WARNING] Only one fast thread at most, more are likely to cause errors\n");
	}

	printf_verbose("running test for %lu seconds, %u fast threads, "
		       "%u slow threads.\n",
		       duration, nr_fast, nr_slow);
	printf_verbose("Fast class c.s. len: %lu loops.\n", fast_cs_len);
	printf_verbose("Slow class c.s. len: %lu loops.\n", slow_cs_len);
	printf_verbose("Fast class delay: %lu loops.\n", fast_delay);
	printf_verbose("Slow class delay: %lu loops.\n", slow_delay);
	printf_verbose("thread %-6s, tid %lu\n",
			"main", urcu_get_thread_id());

	tid_fast = calloc(nr_fast, sizeof(*tid_fast));
	tid_slow = calloc(nr_slow, sizeof(*tid_slow));
	count_fast = calloc(nr_fast, sizeof(*count_fast));
	count_slow = calloc(nr_slow,  sizeof(*count_slow));

	next_aff = 0;

	set_signal_handler();

	for (i = 0; i < nr_fast; i++) {
		err = pthread_create(&tid_fast[i], NULL, thr_fast,
				     &count_fast[i]);
		if (err != 0)
			exit(1);
	}
	for (i = 0; i < nr_slow; i++) {
		err = pthread_create(&tid_slow[i], NULL, thr_slow,
				     &count_slow[i]);
		if (err != 0)
			exit(1);
	}

	cmm_smp_mb();

	test_go = 1;

#if 0
	for (i = 0; i < duration; i++) {
		sleep(1);
		if (verbose_mode)
			(void) write(1, ".", 1);
	}
#endif
	clock_gettime(CLOCK_MONOTONIC, &init_time);
	for (;;) {
		struct timespec cur_time;
		int i;

		for (i = 0; i < nr_fast; i++)
			pthread_kill(tid_fast[i], SIGUSR1);
		for (i = 0; i < nr_slow; i++)
			pthread_kill(tid_slow[i], SIGUSR1);
		clock_gettime(CLOCK_MONOTONIC, &cur_time);
		if (cur_time.tv_sec > init_time.tv_sec + duration)
			break;
	}

	test_stop = 1;

	for (i = 0; i < nr_fast; i++) {
		err = pthread_join(tid_fast[i], &tret);
		if (err != 0)
			exit(1);
		tot_fast_loops += count_fast[i];
	}
	for (i = 0; i < nr_slow; i++) {
		err = pthread_join(tid_slow[i], &tret);
		if (err != 0)
			exit(1);
		tot_slow_loops += count_slow[i];
	}
	
	printf_verbose("total number of loops fast: %llu, slow %llu\n",
		       tot_fast_loops, tot_slow_loops);
	printf("SUMMARY %-25s testdur %4lu nr_fast %3u fast_delay %6lu "
		"nr_slow %3u "
		"slow_delay %6lu nr_fast_loops %12llu nr_slow_loops %12llu "
		"total_loops %12llu\n",
		argv[0], duration, nr_fast, fast_delay,
		nr_slow, slow_delay, tot_fast_loops, tot_slow_loops,
		tot_fast_loops + tot_slow_loops);
	free(count_fast);
	free(count_slow);
	free(tid_fast);
	free(tid_slow);
	return retval;
}
