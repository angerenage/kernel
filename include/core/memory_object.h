#pragma once

#include <core/backing_store.h>
#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum memory_object_type {
	MEMORY_OBJECT_ANONYMOUS = 0,
	MEMORY_OBJECT_EXTERNAL_PHYSICAL,
};

/* A free slot in a memory_object_slab. */
struct memory_object_free_slot {
	struct memory_object_free_slot* next;
};

/* A slab of memory_object control blocks. */
struct memory_object_slab {
	struct memory_object_slab*      next;
	uintptr_t                       phys;
	size_t                          used;
	struct memory_object_free_slot* free_slots;
};

/* Owner of logical memory contents and physical backing. */
struct memory_object {
	struct spinlock            lock;
	enum memory_object_type    type;
	size_t                     page_count;
	uint64_t                   reference_count;
	struct memory_object_slab* slab;
	struct backing_store       backing;
};

/* Initialize the boot-safe control-block allocator after PMM initialization. */
bool memory_object_allocator_init(void);

/* Create an anonymous object and return one retained reference. */
bool memory_object_create_anonymous(size_t page_count, struct memory_object** out_object);

/* Create an externally-backed physical object and return one retained reference. */
bool memory_object_create_external(uintptr_t phys_base, size_t page_count, struct memory_object** out_object);

/* Acquire one object-lifetime reference unless the object is already being destroyed. */
bool memory_object_retain(struct memory_object* object);

/* Release one object-lifetime reference and destroy the object after the final release. */
void memory_object_release(struct memory_object* object);

/* Return whether the object owns anonymous backing or describes external physical backing. */
enum memory_object_type memory_object_type(const struct memory_object* object);

/* Return the number of logical pages represented by the object. */
size_t memory_object_page_count(const struct memory_object* object);

/* Return false when logical_page has no materialized physical backing. */
bool memory_object_page_phys(struct memory_object* object, size_t logical_page, uintptr_t* out_phys);

/* Materialize anonymous backing when needed and report whether this call allocated it. */
bool memory_object_ensure_page(struct memory_object* object, size_t logical_page, uintptr_t* out_phys,
                               bool* out_allocated);

/* Release one newly-created anonymous page during transactional rollback. */
bool memory_object_release_page(struct memory_object* object, size_t logical_page);

/* Release anonymous backing in a logical subrange; external backing is preserved. */
bool memory_object_release_pages(struct memory_object* object, size_t first_page, size_t page_count);
