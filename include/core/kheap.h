#pragma once

#include <base/heap.h>
#include <libc/stdlib.h>

static inline bool kheap_init(void) {
	return heap_init();
}

static inline bool kheap_is_initialized(void) {
	return heap_is_initialized();
}

static inline void* kmalloc(size_t size) {
	return malloc(size);
}

static inline void kfree(void* ptr) {
	free(ptr);
}

static inline void* kcalloc(size_t nmemb, size_t size) {
	return calloc(nmemb, size);
}

static inline void* krealloc(void* ptr, size_t size) {
	return realloc(ptr, size);
}

static inline size_t kheap_total_bytes(void) {
	return heap_total_bytes();
}

static inline size_t kheap_free_bytes(void) {
	return heap_free_bytes();
}
