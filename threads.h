/* SPDX-License-Identifier: MIT */
/* threads.h - how many decoding threads to use by default.
 *
 * The rule: two threads per CPU, at most 16. Each decoding
 * thread works on one frame at a time and needs one frame-sized buffer (at most ZF_MAX_FRAME_RAW), and
 * nothing else sets the memory cap: the memory a mount uses for decoding scales with the number of
 * threads, so a small machine with few CPUs uses little and a large one uses what it can use. */
#ifndef ZFRAG_THREADS_H
#define ZFRAG_THREADS_H

#define ZF_THREADS_PER_CPU      2
#define ZF_THREADS_DEFAULT_CAP  16      /* the cap on the default */
#define ZF_THREADS_LIMIT        64      /* the most anyone may ask for explicitly */

/* The CPUs this process may run on (its affinity mask, so taskset and cgroup cpusets are honoured),
 * falling back to the number online. At least 1. */
unsigned zf_cpu_count(void);

/* The default rule for a given number of CPUs: ZF_THREADS_PER_CPU each, capped; at least 1. */
unsigned zf_threads_for(unsigned cpus);

/* zf_threads_for(zf_cpu_count()) */
unsigned zf_default_threads(void);

#endif
