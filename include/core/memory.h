#pragma once

#include <base/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct memory;

enum memory_span_kind {
	MEMORY_SPAN_HOLE = 0,
	MEMORY_SPAN_PRESENT,
};

/* One present physical run or sparse hole within a memory view. */
struct memory_span {
	enum memory_span_kind kind;
	size_t                offset;
	size_t                size;
	uintptr_t             physical_address;
};

/* Parameters for creating one fixed physical memory root. */
struct memory_physical_request {
	uintptr_t        physical_address;
	size_t           size;
	enum memory_type memory_type;
	bool             external_cpu_accessible;
};

/* Physical placement policy for materializing an anonymous memory range. */
struct memory_materialize_request {
	size_t    offset;
	size_t    size;
	size_t    alignment;
	uintptr_t minimum_address;
	uintptr_t maximum_address;
	bool      require_contiguous;
};

/* Create one sparse anonymous memory with an exact logical byte size. */
bool memory_create_anonymous(size_t size, struct memory** out_memory);

/* Create one fixed physical memory without modifying its existing contents. */
bool memory_create_physical(const struct memory_physical_request* request, struct memory** out_memory);

/* Create a distinct child view over a byte range of its retained parent. */
bool memory_slice(struct memory* parent, size_t offset, size_t size, struct memory** out_memory);

/* Acquire one memory-lifetime reference unless destruction has begun. */
bool memory_retain(struct memory* memory);

/* Release one reference and iteratively destroy an unreferenced parent chain. */
void memory_release(struct memory* memory);

/* Return the exact logical byte size of a memory view. */
size_t memory_size(const struct memory* memory);

/* Return the immutable logical memory type. */
enum memory_type memory_type(const struct memory* memory);

/* Return whether ordinary byte transfers are permitted and physically safe. */
bool memory_can_transfer(const struct memory* memory);

/* Return whether the backing is safe for ordinary direct-map CPU access. */
bool memory_cpu_accessible(const struct memory* memory);

/* Return whether a Memory-relative range and its backing offset satisfy an alignment. */
bool memory_range_is_aligned(const struct memory* memory, size_t offset, size_t size, size_t alignment);

/* Describe the present physical run or sparse hole beginning at an offset. */
bool memory_query(struct memory* memory, size_t offset, size_t maximum_size, struct memory_span* out_span);

/* Materialize one aligned memory-relative range with the requested placement. */
bool memory_materialize(struct memory* memory, const struct memory_materialize_request* request);

/* Copy bytes from a transferable memory view. */
bool memory_read(struct memory* memory, size_t offset, void* destination, size_t size);

/* Copy bytes into a transferable memory view, materializing holes as needed. */
bool memory_write(struct memory* memory, size_t offset, const void* source, size_t size);
