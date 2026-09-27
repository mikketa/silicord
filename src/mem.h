#pragma once
#include <stddef.h>

/* Process heap wrappers. Allocations are zeroed; running out of memory exits the process. */
void *mem_alloc(size_t n);
void *mem_realloc(void *p, size_t n);
void mem_free(void *p);
/* Bytes currently allocated through these functions, and the highest value seen. */
size_t mem_used(void);
size_t mem_peak(void);
