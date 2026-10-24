/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>

#include "threads.h"

unsigned zf_cpu_count(void)
{
	cpu_set_t set;
	long online;

	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof set, &set) == 0) {
		int n = CPU_COUNT(&set);
		if (n > 0) return (unsigned)n;
	}
	online = sysconf(_SC_NPROCESSORS_ONLN);
	return online > 0 ? (unsigned)online : 1;
}

unsigned zf_threads_for(unsigned cpus)
{
	unsigned t = (cpus ? cpus : 1) * ZF_THREADS_PER_CPU;

	return t > ZF_THREADS_DEFAULT_CAP ? ZF_THREADS_DEFAULT_CAP : t;
}

unsigned zf_default_threads(void)
{
	return zf_threads_for(zf_cpu_count());
}
