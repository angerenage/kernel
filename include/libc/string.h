#pragma once

#include <stddef.h>

int   memcmp(const void*, const void*, size_t);
int   strcmp(const char*, const char*);
void* memcpy(void* restrict, const void* restrict, size_t);
void* memmove(void*, const void*, size_t);
void* memset(void*, int, size_t);
#ifdef BASE_STDLIB_RENAME_ALLOC
char* heap_strdup(const char*);
char* heap_strndup(const char*, size_t);

#define strdup heap_strdup
#define strndup heap_strndup
#else
char* strdup(const char*);
char* strndup(const char*, size_t);
#endif
size_t strlen(const char*);
size_t strlcpy(char* restrict, const char* restrict, size_t);
