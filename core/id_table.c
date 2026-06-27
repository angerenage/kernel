#include <core/id_table.h>
#include <core/lock.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	ID_TABLE_INITIAL_CAPACITY = 16u,
};

static enum id_table_result id_table_index(const struct id_table* table, id_table_id_t id, size_t* out_index) {
	id_table_id_t index64;

	if (out_index != NULL) *out_index = 0u;
	if (table == NULL) return ID_TABLE_INVALID_ARGUMENTS;
	if (id < table->min_id || id > table->max_id) return ID_TABLE_OUT_OF_RANGE;

	index64 = id - table->min_id;
	if (index64 > (id_table_id_t)SIZE_MAX) return ID_TABLE_OUT_OF_RANGE;

	if (out_index != NULL) *out_index = (size_t)index64;
	return ID_TABLE_OK;
}

static enum id_table_result id_table_grow_locked(struct id_table* table, size_t min_capacity) {
	void** new_slots;
	size_t old_capacity;
	size_t new_capacity;
	size_t new_bytes;

	if (table == NULL) return ID_TABLE_INVALID_ARGUMENTS;
	if (min_capacity <= table->capacity) return ID_TABLE_OK;

	old_capacity = table->capacity;
	new_capacity = old_capacity == 0u ? ID_TABLE_INITIAL_CAPACITY : old_capacity;
	while (new_capacity < min_capacity) {
		if (new_capacity > SIZE_MAX / 2u) {
			new_capacity = min_capacity;
			break;
		}
		new_capacity *= 2u;
	}
	if (new_capacity > SIZE_MAX / sizeof(*table->slots)) return ID_TABLE_NO_MEMORY;

	new_bytes = new_capacity * sizeof(*table->slots);
	new_slots = realloc(table->slots, new_bytes);
	if (new_slots == NULL) return ID_TABLE_NO_MEMORY;

	if (new_capacity > old_capacity) {
		memset(new_slots + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*new_slots));
	}

	table->slots    = new_slots;
	table->capacity = new_capacity;
	return ID_TABLE_OK;
}

enum id_table_result id_table_init(struct id_table* table, const char* lock_name, id_table_id_t min_id,
                                   id_table_id_t max_id) {
	if (table == NULL || min_id == ID_TABLE_ID_INVALID || min_id > max_id) return ID_TABLE_INVALID_ARGUMENTS;

	*table = (struct id_table){
		.next_id = min_id,
		.min_id  = min_id,
		.max_id  = max_id,
	};
	spinlock_init_class(&table->lock, lock_name, SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE);
	return ID_TABLE_OK;
}

void id_table_deinit(struct id_table* table) {
	struct irq_state state;
	void**           slots;

	if (table == NULL) return;

	state           = spinlock_lock_irqsave(&table->lock);
	slots           = table->slots;
	table->next_id  = ID_TABLE_ID_INVALID;
	table->min_id   = ID_TABLE_ID_INVALID;
	table->max_id   = ID_TABLE_ID_INVALID;
	table->count    = 0u;
	table->capacity = 0u;
	table->slots    = NULL;
	spinlock_unlock_irqrestore(&table->lock, state);

	free(slots);
}

enum id_table_result id_table_alloc(struct id_table* table, void* object, id_table_id_t* out_id) {
	struct irq_state     state;
	id_table_id_t        id;
	size_t               index;
	enum id_table_result result;

	if (out_id != NULL) *out_id = ID_TABLE_ID_INVALID;
	if (table == NULL || object == NULL || out_id == NULL) return ID_TABLE_INVALID_ARGUMENTS;

	state = spinlock_lock_irqsave(&table->lock);
	id    = table->next_id;
	if (id < table->min_id || id > table->max_id) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return ID_TABLE_EXHAUSTED;
	}
	result = id_table_index(table, id, &index);
	if (result != ID_TABLE_OK) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return result;
	}
	if (index == SIZE_MAX) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return ID_TABLE_EXHAUSTED;
	}
	result = id_table_grow_locked(table, index + 1u);
	if (result != ID_TABLE_OK) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return result;
	}

	table->slots[index] = object;
	table->count++;
	if (table->next_id < table->max_id) {
		table->next_id++;
	}
	else {
		table->next_id = ID_TABLE_ID_INVALID;
	}
	spinlock_unlock_irqrestore(&table->lock, state);

	*out_id = id;
	return ID_TABLE_OK;
}

void* id_table_lookup(struct id_table* table, id_table_id_t id) {
	struct irq_state state;
	size_t           index;
	void*            object = NULL;

	if (id_table_index(table, id, &index) != ID_TABLE_OK) return NULL;

	state = spinlock_lock_irqsave(&table->lock);
	if (index < table->capacity) object = table->slots[index];
	spinlock_unlock_irqrestore(&table->lock, state);
	return object;
}

void* id_table_lookup_retain(struct id_table* table, id_table_id_t id, id_table_retain_fn retain, void* context) {
	struct irq_state state;
	size_t           index;
	void*            object = NULL;

	if (retain == NULL || id_table_index(table, id, &index) != ID_TABLE_OK) return NULL;

	state = spinlock_lock_irqsave(&table->lock);
	if (index < table->capacity) object = table->slots[index];
	if (object != NULL && !retain(object, context)) object = NULL;
	spinlock_unlock_irqrestore(&table->lock, state);
	return object;
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
	if (index >= table->capacity || table->slots[index] == NULL) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return ID_TABLE_NOT_FOUND;
	}

	object              = table->slots[index];
	table->slots[index] = NULL;
	if (table->count != 0u) table->count--;
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
