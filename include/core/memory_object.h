#pragma once

#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum memory_object_type {
	MEMORY_OBJECT_OWNED = 0,
	MEMORY_OBJECT_EXTERNAL,
};

/* Owner of logical memory contents and physical backing. */
struct memory_object {
	struct spinlock lock;
	uint8_t         radix_depth;
	uint8_t         type;
	size_t          page_count;
	uintptr_t       backing_root_or_phys;
	uint64_t        reference_count;
};

/* Create an owned logical memory object and return one retained reference. */
bool memory_object_create_owned(size_t page_count, struct memory_object** out_object);

/* Create an externally backed physical memory object and return one retained reference. */
bool memory_object_create_external(uintptr_t phys_base, size_t page_count, struct memory_object** out_object);

/* Acquire one object-lifetime reference unless the object is being destroyed. */
bool memory_object_retain(struct memory_object* object);

/* Release one object-lifetime reference and destroy the object after the final release. */
void memory_object_release(struct memory_object* object);

/* Return the object's backing type. */
enum memory_object_type memory_object_type(const struct memory_object* object);

/* Return the object's logical size in pages. */
size_t memory_object_page_count(const struct memory_object* object);

/* Return a materialized page's physical address, or false when it has no backing. */
bool memory_object_page_phys(struct memory_object* object, size_t logical_page, uintptr_t* out_phys);

/* Return a logical page's physical address, materializing owned backing when needed. */
bool memory_object_resolve_page(struct memory_object* object, size_t logical_page, uintptr_t* out_phys);

/* Read bytes from the object's logical contents. */
bool memory_object_read(struct memory_object* object, size_t byte_offset, void* dst, size_t size);

/* Write bytes to the object's logical contents. */
bool memory_object_write(struct memory_object* object, size_t byte_offset, const void* src, size_t size);
