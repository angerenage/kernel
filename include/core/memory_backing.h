#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum memory_backing_kind {
	MEMORY_BACKING_ANONYMOUS = 0,
	MEMORY_BACKING_PHYSICAL,
};

enum memory_backing_span_kind {
	MEMORY_BACKING_SPAN_HOLE = 0,
	MEMORY_BACKING_SPAN_PRESENT,
};

struct memory_backing;

/* Parameters for claiming one fixed physical byte range. */
struct memory_backing_physical_request {
	uintptr_t physical_address;
	size_t    size;
	/* Applies only when the range is outside PMM-managed memory. */
	bool external_cpu_accessible;
};

/* One present physical run or implicit sparse hole. */
struct memory_backing_span {
	enum memory_backing_span_kind kind;
	size_t                        offset;
	size_t                        size;
	uintptr_t                     physical_address;
};

/* Physical placement policy for anonymous materialization. */
struct memory_backing_materialize_request {
	size_t offset;
	size_t size;
	/* Zero selects the PMM allocation granule. */
	size_t alignment;
	/* Inclusive lower bound and exclusive upper bound; zero means unbounded. */
	uintptr_t minimum_address;
	uintptr_t maximum_address;
	bool      require_contiguous;
};

/* Create sparse, zero-filled storage with byte capacity size. */
bool memory_backing_create_anonymous(size_t size, struct memory_backing** out_backing);

/* Atomically claim one fixed physical byte range. */
bool memory_backing_create_physical(const struct memory_backing_physical_request* request,
                                    struct memory_backing**                       out_backing);

/* Acquire one backing-lifetime reference unless destruction has begun. */
bool memory_backing_retain(struct memory_backing* backing);

/* Release one backing-lifetime reference and destroy after the final release. */
void memory_backing_release(struct memory_backing* backing);

/* Return the backing's logical byte capacity. */
size_t memory_backing_size(const struct memory_backing* backing);

/* Return whether the backing is anonymous or fixed physical storage. */
enum memory_backing_kind memory_backing_kind(const struct memory_backing* backing);

/* Return whether ordinary byte copies through the direct map are safe. */
bool memory_backing_cpu_accessible(const struct memory_backing* backing);

/* Describe the present run or implicit hole beginning at offset. */
bool memory_backing_query(struct memory_backing* backing, size_t offset, size_t maximum_size,
                          struct memory_backing_span* out_span);

/* Materialize anonymous storage according to the requested physical placement policy. */
bool memory_backing_materialize(struct memory_backing*                           backing,
                                const struct memory_backing_materialize_request* request);

/* Copy bytes from present storage and synthesize zeroes for sparse holes. */
bool memory_backing_read(struct memory_backing* backing, size_t offset, void* destination, size_t size);

/* Materialize the required coverage and copy bytes into the backing. */
bool memory_backing_write(struct memory_backing* backing, size_t offset, const void* source, size_t size);
