#pragma once

#include <stddef.h>

long   strtol(const char* restrict string, char** restrict end, int base);
double strtod(const char* restrict string, char** restrict end);
void   qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));

int  abs(int value);
long labs(long value);

#ifdef BASE_STDLIB_RENAME_ALLOC
void* heap_malloc(size_t);
void* heap_calloc(size_t, size_t);
void* heap_realloc(void*, size_t);
void  heap_free(void*);

#define malloc heap_malloc
#define calloc heap_calloc
#define realloc heap_realloc
#define free heap_free
#else
void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void  free(void*);
#endif
