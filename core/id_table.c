#include <core/id_table.h>
#include <core/lock.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	ID_TABLE_INITIAL_CAPACITY = 16u,
	ID_TABLE_BITMAP_BITS      = sizeof(uint64_t) * 8u,
};

union id_table_slot {
	void*  object;
	size_t next_free;
};

static size_t id_table_bitmap_words(size_t capacity) {
	return capacity / ID_TABLE_BITMAP_BITS + (capacity % ID_TABLE_BITMAP_BITS != 0u ? 1u : 0u);
}

static size_t id_table_slots_offset(size_t capacity) {
	size_t bitmap_bytes = id_table_bitmap_words(capacity) * sizeof(uint64_t);
	size_t alignment    = _Alignof(union id_table_slot);
	return (bitmap_bytes + alignment - 1u) & ~(alignment - 1u);
}

static uint64_t* id_table_bitmap(const struct id_table* table) {
	return (uint64_t*)table->storage;
}

static bool id_table_slot_is_allocated(const struct id_table* table, size_t index) {
	uint64_t* bitmap = id_table_bitmap(table);
	return (bitmap[index / ID_TABLE_BITMAP_BITS] & (1ull << (index % ID_TABLE_BITMAP_BITS))) != 0u;
}

static void id_table_slot_set_allocated(struct id_table* table, size_t index) {
	uint64_t* bitmap = id_table_bitmap(table);
	bitmap[index / ID_TABLE_BITMAP_BITS] |= 1ull << (index % ID_TABLE_BITMAP_BITS);
}

static void id_table_slot_set_free(struct id_table* table, size_t index) {
	uint64_t* bitmap = id_table_bitmap(table);
	bitmap[index / ID_TABLE_BITMAP_BITS] &= ~(1ull << (index % ID_TABLE_BITMAP_BITS));
}

static enum id_table_result id_table_index(const struct id_table* table, id_table_id_t id, size_t* out_index) {
	id_table_id_t index;

	if (out_index != NULL) *out_index = 0u;
	if (table == NULL) return ID_TABLE_INVALID_ARGUMENTS;
	if (id < table->min_id || id > table->max_id) return ID_TABLE_OUT_OF_RANGE;

	index = id - table->min_id;
	if (index > (id_table_id_t)SIZE_MAX) return ID_TABLE_OUT_OF_RANGE;
	if (out_index != NULL) *out_index = (size_t)index;
	return ID_TABLE_OK;
}

static bool id_table_id_for_index(const struct id_table* table, size_t index, id_table_id_t* out_id) {
	id_table_id_t offset = (id_table_id_t)index;

	if (table->min_id == ID_TABLE_ID_INVALID || table->min_id > table->max_id || index == SIZE_MAX ||
	    offset > table->max_id - table->min_id)
		return false;
	*out_id = table->min_id + offset;
	return true;
}

static enum id_table_result id_table_grow_locked(struct id_table* table, size_t minimum_capacity) {
	void*                storage;
	size_t               old_capacity;
	size_t               old_slots_offset;
	size_t               old_bitmap_bytes;
	size_t               new_capacity;
	size_t               new_slots_offset;
	size_t               new_bitmap_bytes;
	size_t               slot_bytes;
	size_t               storage_bytes;
	union id_table_slot* old_slots;
	union id_table_slot* new_slots;

	if (minimum_capacity <= table->capacity) return ID_TABLE_OK;

	old_capacity = table->capacity;
	new_capacity = old_capacity == 0u ? ID_TABLE_INITIAL_CAPACITY : old_capacity;
	while (new_capacity < minimum_capacity) {
		if (new_capacity > SIZE_MAX / 2u) {
			new_capacity = minimum_capacity;
			break;
		}
		new_capacity *= 2u;
	}
	id_table_id_t maximum_index = table->max_id - table->min_id;
	if (maximum_index < (id_table_id_t)SIZE_MAX && new_capacity > (size_t)maximum_index + 1u) {
		new_capacity = (size_t)maximum_index + 1u;
	}
	if (new_capacity > SIZE_MAX / sizeof(union id_table_slot)) return ID_TABLE_NO_MEMORY;

	new_slots_offset = id_table_slots_offset(new_capacity);
	new_bitmap_bytes = id_table_bitmap_words(new_capacity) * sizeof(uint64_t);
	slot_bytes       = new_capacity * sizeof(union id_table_slot);
	if (new_slots_offset > SIZE_MAX - slot_bytes) return ID_TABLE_NO_MEMORY;
	storage_bytes = new_slots_offset + slot_bytes;

	old_slots_offset = id_table_slots_offset(old_capacity);
	old_bitmap_bytes = id_table_bitmap_words(old_capacity) * sizeof(uint64_t);
	storage          = realloc(table->storage, storage_bytes);
	if (storage == NULL) return ID_TABLE_NO_MEMORY;

	old_slots = (union id_table_slot*)((uint8_t*)storage + old_slots_offset);
	new_slots = (union id_table_slot*)((uint8_t*)storage + new_slots_offset);
	if (old_capacity != 0u) memmove(new_slots, old_slots, old_capacity * sizeof(*new_slots));
	if (new_bitmap_bytes > old_bitmap_bytes) {
		memset((uint8_t*)storage + old_bitmap_bytes, 0, new_bitmap_bytes - old_bitmap_bytes);
	}
	memset(new_slots + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*new_slots));

	table->storage  = storage;
	table->slots    = new_slots;
	table->capacity = new_capacity;
	return ID_TABLE_OK;
}

enum id_table_result id_table_init(struct id_table* table, const char* lock_name, id_table_id_t min_id,
                                   id_table_id_t max_id) {
	if (table == NULL || min_id == ID_TABLE_ID_INVALID || min_id > max_id) return ID_TABLE_INVALID_ARGUMENTS;

	*table = (struct id_table){
		.min_id    = min_id,
		.max_id    = max_id,
		.free_head = SIZE_MAX,
	};
	spinlock_init_class(&table->lock, lock_name, SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE);
	return ID_TABLE_OK;
}

void id_table_deinit(struct id_table* table) {
	struct irq_state state;
	void*            storage;

	if (table == NULL) return;

	state                 = spinlock_lock_irqsave(&table->lock);
	storage               = table->storage;
	table->min_id         = ID_TABLE_ID_INVALID;
	table->max_id         = ID_TABLE_ID_INVALID;
	table->count          = 0u;
	table->capacity       = 0u;
	table->high_watermark = 0u;
	table->free_head      = SIZE_MAX;
	table->storage        = NULL;
	table->slots          = NULL;
	spinlock_unlock_irqrestore(&table->lock, state);

	free(storage);
}

static enum id_table_result id_table_alloc_internal(struct id_table* table, void* object, id_table_id_t* out_id,
                                                    bool reuse) {
	struct irq_state     state;
	id_table_id_t        id;
	size_t               index;
	enum id_table_result result;

	if (out_id != NULL) *out_id = ID_TABLE_ID_INVALID;
	if (table == NULL || object == NULL || out_id == NULL) return ID_TABLE_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&table->lock);
	if (reuse && table->free_head != SIZE_MAX) {
		index            = table->free_head;
		table->free_head = table->slots[index].next_free;
	}
	else {
		index = table->high_watermark;
		if (!id_table_id_for_index(table, index, &id)) {
			spinlock_unlock_irqrestore(&table->lock, state);
			return ID_TABLE_EXHAUSTED;
		}
		result = id_table_grow_locked(table, index + 1u);
		if (result != ID_TABLE_OK) {
			spinlock_unlock_irqrestore(&table->lock, state);
			return result;
		}
		table->high_watermark++;
	}

	if (!id_table_id_for_index(table, index, &id)) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return ID_TABLE_EXHAUSTED;
	}
	table->slots[index].object = object;
	id_table_slot_set_allocated(table, index);
	table->count++;
	spinlock_unlock_irqrestore(&table->lock, state);

	*out_id = id;
	return ID_TABLE_OK;
}

enum id_table_result id_table_alloc(struct id_table* table, void* object, id_table_id_t* out_id) {
	return id_table_alloc_internal(table, object, out_id, true);
}

enum id_table_result id_table_alloc_fresh(struct id_table* table, void* object, id_table_id_t* out_id) {
	return id_table_alloc_internal(table, object, out_id, false);
}

void* id_table_lookup(struct id_table* table, id_table_id_t id) {
	struct irq_state state;
	size_t           index;
	void*            object = NULL;

	if (id_table_index(table, id, &index) != ID_TABLE_OK) return NULL;

	state = spinlock_lock_irqsave(&table->lock);
	if (index < table->high_watermark && id_table_slot_is_allocated(table, index)) {
		object = table->slots[index].object;
	}
	spinlock_unlock_irqrestore(&table->lock, state);
	return object;
}

void* id_table_lookup_retain(struct id_table* table, id_table_id_t id, id_table_retain_fn retain, void* context) {
	struct irq_state state;
	size_t           index;
	void*            object = NULL;

	if (retain == NULL || id_table_index(table, id, &index) != ID_TABLE_OK) return NULL;

	state = spinlock_lock_irqsave(&table->lock);
	if (index < table->high_watermark && id_table_slot_is_allocated(table, index)) {
		object = table->slots[index].object;
	}
	if (object != NULL && !retain(object, context)) object = NULL;
	spinlock_unlock_irqrestore(&table->lock, state);
	return object;
}

size_t id_table_extent_locked(const struct id_table* table) {
	return table == NULL ? 0u : table->high_watermark;
}

void* id_table_lookup_index_locked(const struct id_table* table, size_t index) {
	if (table == NULL || index >= table->high_watermark || !id_table_slot_is_allocated(table, index)) return NULL;
	return table->slots[index].object;
}

enum id_table_result id_table_remove(struct id_table* table, id_table_id_t id, void** out_object) {
	struct irq_state     state;
	size_t               index;
	void*                object;
	enum id_table_result result;

	if (out_object != NULL) *out_object = NULL;
	result = id_table_index(table, id, &index);
	if (result != ID_TABLE_OK) return result;

	state = spinlock_lock_irqsave(&table->lock);
	if (index >= table->high_watermark || !id_table_slot_is_allocated(table, index)) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return ID_TABLE_NOT_FOUND;
	}

	object = table->slots[index].object;
	id_table_slot_set_free(table, index);
	table->slots[index].next_free = table->free_head;
	table->free_head              = index;
	table->count--;
	spinlock_unlock_irqrestore(&table->lock, state);

	if (out_object != NULL) *out_object = object;
	return ID_TABLE_OK;
}

size_t id_table_count(struct id_table* table) {
	struct irq_state state;
	size_t           count;

	if (table == NULL) return 0u;

	state = spinlock_lock_irqsave(&table->lock);
	count = table->count;
	spinlock_unlock_irqrestore(&table->lock, state);
	return count;
}
