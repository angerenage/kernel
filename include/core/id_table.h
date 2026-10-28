#pragma once

#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t id_table_id_t;

#define ID_TABLE_ID_INVALID ((id_table_id_t)0u)

union id_table_slot;

struct id_table {
	struct spinlock      lock;
	id_table_id_t        min_id;
	id_table_id_t        max_id;
	size_t               count;
	size_t               capacity;
	size_t               high_watermark;
	size_t               free_head;
	void*                storage;
	union id_table_slot* slots;
};

#define ID_TABLE_INITIALIZER(table_name, minimum_id, maximum_id)                                                       \
	{                                                                                                                  \
		.lock      = SPINLOCK_INIT_CLASS((table_name), SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),                \
		.min_id    = (minimum_id),                                                                                     \
		.max_id    = (maximum_id),                                                                                     \
		.free_head = SIZE_MAX,                                                                                         \
	}

/* Called while the table lock protects object from concurrent removal. Return false to reject the lookup. */
typedef bool (*id_table_retain_fn)(void* object, void* context);

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
 * Initialize a synchronized ID table. IDs are assigned sequentially until an
 * entry is removed, after which vacant IDs are recycled.
 *
 * The table stores object pointers but does not own them. A successful lookup
 * only proves that the object was registered at that instant; callers that need
 * stronger lifetime guarantees must add object-specific reference counting.
 */
enum id_table_result id_table_init(struct id_table* table, const char* lock_name, id_table_id_t min_id,
                                   id_table_id_t max_id);

/* Release table storage. Registered objects are left untouched. */
void id_table_deinit(struct id_table* table);

/* Allocate an ID and associate it with object, preferring a recycled ID. */
enum id_table_result id_table_alloc(struct id_table* table, void* object, id_table_id_t* out_id);

/* Allocate an ID that has never previously been returned by this table. */
enum id_table_result id_table_alloc_fresh(struct id_table* table, void* object, id_table_id_t* out_id);

/* Return the object registered for id, or NULL when id is invalid or absent. */
void* id_table_lookup(struct id_table* table, id_table_id_t id);

/* Look up id and retain the object before releasing the table lock. */
void* id_table_lookup_retain(struct id_table* table, id_table_id_t id, id_table_retain_fn retain, void* context);

/* Return the number of currently registered objects. The caller must hold the table lock. */
size_t id_table_extent_locked(const struct id_table* table);

/* Look up the object at the given index in the table. The caller must hold the table lock. */
void* id_table_lookup_index_locked(const struct id_table* table, size_t index);

/* Remove id from the table and optionally return the object that was stored. */
enum id_table_result id_table_remove(struct id_table* table, id_table_id_t id, void** out_object);

/* Return the number of currently registered objects. */
size_t id_table_count(struct id_table* table);
