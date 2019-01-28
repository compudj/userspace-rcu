/*
 * rseq.c
 *
 * Copyright (C) 2016 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syscall.h>
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <limits.h>
#include <urcu/compiler.h>

#include <urcu/rseq.h>

enum rseq_register_state {
	RSEQ_REGISTER_ALLOWED = 0,
	RSEQ_REGISTER_NESTED = 1,
	RSEQ_REGISTER_EXITING = 2,
};

struct rseq_lib_abi {
	uint32_t register_state;	/* enum rseq_register_state */
	uint32_t refcount;
};

__thread volatile struct rseq __rseq_abi = {
	.cpu_id = RSEQ_CPU_ID_UNINITIALIZED,
};

__thread volatile struct rseq_lib_abi __rseq_lib_abi;

static pthread_key_t rseq_key;

static int sys_rseq(volatile struct rseq *rseq_abi, uint32_t rseq_len,
		    int flags, uint32_t sig)
{
	return syscall(__NR_rseq, rseq_abi, rseq_len, flags, sig);
}

static int rseq_register_current_thread(void)
{
	int rc, ret = 0;

	/*
	 * Nested signal handlers need to check whether registration is
	 * allowed.
	 */
	if (__rseq_lib_abi.register_state != RSEQ_REGISTER_ALLOWED)
		return -1;
	__rseq_lib_abi.register_state = RSEQ_REGISTER_NESTED;
	if (__rseq_lib_abi.refcount == UINT_MAX) {
		ret = -1;
		goto end;
	}
	if (__rseq_lib_abi.refcount++)
		goto end;
	rc = sys_rseq(&__rseq_abi, sizeof(struct rseq), 0, RSEQ_SIG);
	if (!rc) {
		assert(rseq_current_cpu_raw() >= 0);
		goto end;
	}
	if (errno != EBUSY)
		__rseq_abi.cpu_id = RSEQ_CPU_ID_REGISTRATION_FAILED;
	ret = -1;
	__rseq_lib_abi.refcount--;
end:
	__rseq_lib_abi.register_state = RSEQ_REGISTER_ALLOWED;
	return ret;
}

static int urcu_rseq_unregister_current_thread(void)
{
	int rc, ret = 0;

	rc = rseq_unregister_current_thread();
	if (rc) {
		ret = -1;
		goto end;
	}
	rseq_registered = 0;
end:
	return ret;
}

static void urcu_destroy_rseq_key(void *key)
{
	if (urcu_rseq_unregister_current_thread())
		abort();
}

static int rseq_unregister_current_thread(void)
{
	int rc, ret = 0;

	if (__rseq_lib_abi.register_state != RSEQ_REGISTER_ALLOWED)
		return -1;
	__rseq_lib_abi.register_state = RSEQ_REGISTER_NESTED;
	if (!__rseq_lib_abi.refcount) {
		ret = -1;
		goto end;
	}
	if (--__rseq_lib_abi.refcount)
		goto end;
	rc = sys_rseq(&__rseq_abi, sizeof(struct rseq),
		      RSEQ_FLAG_UNREGISTER, RSEQ_SIG);
	if (!rc)
		goto end;
	ret = -1;
end:
	__rseq_lib_abi.register_state = RSEQ_REGISTER_ALLOWED;
	return ret;
}

int urcu_rseq_register_current_thread(void)
{
	int rc, ret = 0;

	rc = rseq_register_current_thread();
	if (rc) {
		ret = -1;
		goto end;
	}
	/*
	 * Register destroy notifier. Pointer needs to
	 * be non-NULL.
	 */
	if (pthread_setspecific(rseq_key, (void *)0x1))
		abort();
end:
	return ret;
}

static void __attribute__((constructor)) urcu_rseq_init(void)
{
	int ret;

	ret = pthread_key_create(&rseq_key, urcu_destroy_rseq_key);
	if (ret) {
		errno = -ret;
		perror("pthread_key_create");
		abort();
	}
}

static void __attribute__((destructor)) urcu_rseq_destroy(void)
{
	int ret;

	ret = pthread_key_delete(rseq_key);
	if (ret) {
		errno = -ret;
		perror("pthread_key_delete");
		abort();
	}
}
