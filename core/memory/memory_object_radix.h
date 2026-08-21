#pragma once

#include <core/memory_object.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEMORY_OBJECT_RADIX_BITS 9u
#define MEMORY_OBJECT_RADIX_ENTRIES 512u
#define MEMORY_OBJECT_RADIX_MAX_DEPTH 6u

/* Return the radix depth needed for a logical page count. */
uint8_t memory_object_radix_depth(size_t page_count);

/* Look up an existing physical page in an owned object's radix. */
bool memory_object_radix_lookup(const struct memory_object* object, size_t page_index, uintptr_t* out_phys);

/* Resolve a physical page in an owned object's radix. */
bool memory_object_radix_resolve(struct memory_object* object, size_t page_index, uintptr_t* out_phys);

/* Release every page and node owned by an owned object's radix. */
void memory_object_radix_release(struct memory_object* object);
