/* SPDX-License-Identifier: MIT */
/* A minimal assertion helper for the host tests */
#ifndef CHECK_H
#define CHECK_H
#include <stdio.h>
static int check_failures;
#define CHECK(cond, ...) do { if (!(cond)) { check_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_DONE(name) (printf("%s: %s\n", name, check_failures ? "FAILED" : "ok"), check_failures != 0)
#endif
