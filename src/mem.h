#pragma once
#include <stddef.h>

/* Process heap wrappers. Allocations are zeroed; running out of memory exits the process. */
void *mem_alloc(size_t n);
void *mem_realloc(void *p, size_t n);
void mem_free(void *p);
