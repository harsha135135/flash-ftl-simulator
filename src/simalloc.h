/* SPDX-License-Identifier: MIT */
/*
 * Allocation wrapper with a failure-injection hook, so tests can prove that
 * every constructor unwinds cleanly (checked by LeakSanitizer) when any
 * individual allocation fails.
 */
#ifndef SIMALLOC_H
#define SIMALLOC_H

#include <stddef.h>

/* Fail the Nth allocation from now (1 = next); 0 disables injection. */
void sim_alloc_fail_after(unsigned long n);
void *sim_calloc(size_t nmemb, size_t size);

#endif
