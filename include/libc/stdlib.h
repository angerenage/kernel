#pragma once

#include <stddef.h>

#ifdef BASE_STDLIB_RENAME_ALLOC
void* kheap_malloc(size_t);
void* kheap_calloc(size_t, size_t);
void* kheap_realloc(void*, size_t);
void  kheap_free(void*);

#define malloc kheap_malloc
#define calloc kheap_calloc
#define realloc kheap_realloc
#define free kheap_free
#else
void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void  free(void*);
#endif
