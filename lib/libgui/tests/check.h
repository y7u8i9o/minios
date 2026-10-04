#pragma once
#include <stdio.h>
extern int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define RUN(t) do { fprintf(stderr, "running %s\n", #t); t(); } while (0)
