#pragma once

#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t id_table_id_t;

#define ID_TABLE_ID_INVALID ((id_table_id_t)0u)

struct id_table {
	struct spinlock lock;
	id_table_id_t   next_id;
	id_table_id_t   min_id;
	id_table_id_t   max_id;
	size_t          count;
	size_t          capacity;
	void**          slots;
};

enum id_table_result {
	ID_TABLE_OK = 0,
	ID_TABLE_INVALID_ARGUMENTS,
	ID_TABLE_OUT_OF_RANGE,
	ID_TABLE_NOT_FOUND,
	ID_TABLE_EXHAUSTED,
	ID_TABLE_NO_MEMORY,
};

static inline bool id_table_result_is_success(enum id_table_result result) {
	return result == ID_TABLE_OK;
}

/*
 * Initialize a synchronized sequential ID table.
 *
 * The table stores object pointers but does not own them. A successful lookup
 * only proves that the object was registered at that instant; callers that need
 * stronger lifetime guarantees must add object-specific reference counting.
 */
enum id_table_result id_table_init(struct id_table* table, const char* lock_name, id_table_id_t min_id,
                                   id_table_id_t max_id);

/* Release table storage. Registered objects are left untouched. */
void id_table_deinit(struct id_table* table);

/* Allocate the next sequential ID and associate it with object. */
enum id_table_result id_table_alloc(struct id_table* table, void* object, id_table_id_t* out_id);

/* Return the object registered for id, or NULL when id is invalid or absent. */
void* id_table_lookup(struct id_table* table, id_table_id_t id);

/* Remove id from the table and optionally return the object that was stored. */
enum id_table_result id_table_remove(struct id_table* table, id_table_id_t id, void** out_object);

/* Return the number of currently registered objects. */
size_t id_table_count(struct id_table* table);
