/* SPDX-License-Identifier: MIT */
#ifndef TESTUTIL_H
#define TESTUTIL_H

#include <stdio.h>
#include <string.h>

static int t_failures;
static const char *t_current;

#define EXPECT(cond, ...)                                                      \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "  FAIL %s (%s:%d): %s: ", t_current,  \
				__FILE__, __LINE__, #cond);                    \
			fprintf(stderr, " " __VA_ARGS__);                      \
			fprintf(stderr, "\n");                                 \
			t_failures++;                                          \
			return;                                                \
		}                                                              \
	} while (0)

#define RUN(fn)                                                                \
	do {                                                                   \
		int before_ = t_failures;                                      \
		t_current = #fn;                                               \
		fn();                                                          \
		printf("%s %s\n", t_failures == before_ ? "ok  " : "FAIL", #fn); \
		fflush(stdout);                                                \
	} while (0)

#define DONE()                                                                 \
	do {                                                                   \
		printf("%s: %s\n", __FILE__, t_failures ? "FAILED" : "all passed"); \
		return t_failures ? 1 : 0;                                     \
	} while (0)

#endif
