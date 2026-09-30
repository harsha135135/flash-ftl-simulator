// SPDX-License-Identifier: MIT
#include "simalloc.h"

#include <stdlib.h>

static unsigned long fail_countdown;

void sim_alloc_fail_after(unsigned long n)
{
	fail_countdown = n;
}

void *sim_calloc(size_t nmemb, size_t size)
{
	if (fail_countdown && --fail_countdown == 0)
		return NULL;
	return calloc(nmemb, size);
}
