#pragma once

#include <base/memory.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mapping;

/* CPU access rights attached to one Mapping projection. */
typedef memory_access_t mapping_access_t;

#define MAPPING_ACCESS_READ MEMORY_ACCESS_READ
#define MAPPING_ACCESS_WRITE MEMORY_ACCESS_WRITE
#define MAPPING_ACCESS_EXEC MEMORY_ACCESS_EXEC
#define MAPPING_ACCESS_VALID_MASK MEMORY_ACCESS_VALID_MASK

/* Retain one stable Mapping metadata reference. */
bool mapping_retain(struct mapping* mapping);

/* Release one Mapping metadata reference. */
void mapping_release(struct mapping* mapping);

/* Return the usable virtual base cached by Mapping. */
uintptr_t mapping_address(const struct mapping* mapping);

/* Return the complete mapped Memory size in bytes. */
size_t mapping_size(const struct mapping* mapping);

/* Return the reserved guard size preceding Mapping. */
size_t mapping_guard_before(const struct mapping* mapping);

/* Return the reserved guard size following Mapping. */
size_t mapping_guard_after(const struct mapping* mapping);

/* Return the current Mapping access, or its last access after detachment. */
mapping_access_t mapping_access(const struct mapping* mapping);

/* Return the immutable Memory type cached by Mapping. */
enum memory_type mapping_memory_type(const struct mapping* mapping);
