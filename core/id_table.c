#include <core/id_table.h>
#include <core/kheap.h>
#include <core/lock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	ID_TABLE_INITIAL_CAPACITY = 16u,
};

static bool id_table_index(const struct id_table* table, id_table_id_t id, size_t* out_index) {
	id_table_id_t index64;

	if (out_index != NULL) *out_index = 0u;
	if (table == NULL || id < table->min_id || id > table->max_id) return false;

	index64 = id - table->min_id;
	if (index64 > (id_table_id_t)SIZE_MAX) return false;

	if (out_index != NULL) *out_index = (size_t)index64;
	return true;
}

static bool id_table_grow_locked(struct id_table* table, size_t min_capacity) {
	void** new_slots;
	size_t old_capacity;
	size_t new_capacity;
	size_t new_bytes;

	if (table == NULL) return false;
	if (min_capacity <= table->capacity) return true;

	old_capacity = table->capacity;
	new_capacity = old_capacity == 0u ? ID_TABLE_INITIAL_CAPACITY : old_capacity;
	while (new_capacity < min_capacity) {
		if (new_capacity > SIZE_MAX / 2u) {
			new_capacity = min_capacity;
			break;
		}
		new_capacity *= 2u;
	}
	if (new_capacity > SIZE_MAX / sizeof(*table->slots)) return false;

	new_bytes = new_capacity * sizeof(*table->slots);
	new_slots = krealloc(table->slots, new_bytes);
	if (new_slots == NULL) return false;

	if (new_capacity > old_capacity) {
		memset(new_slots + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*new_slots));
	}

	table->slots    = new_slots;
	table->capacity = new_capacity;
	return true;
}

bool id_table_init(struct id_table* table, const char* lock_name, id_table_id_t min_id, id_table_id_t max_id) {
	if (table == NULL || min_id == ID_TABLE_ID_INVALID || min_id > max_id) return false;

	*table = (struct id_table){
		.next_id = min_id,
		.min_id  = min_id,
		.max_id  = max_id,
	};
	spinlock_init_class(&table->lock, lock_name, SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE);
	return true;
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

	kfree(slots);
}

bool id_table_alloc(struct id_table* table, void* object, id_table_id_t* out_id) {
	struct irq_state state;
	id_table_id_t    id;
	size_t           index;

	if (out_id != NULL) *out_id = ID_TABLE_ID_INVALID;
	if (table == NULL || object == NULL || out_id == NULL) return false;

	state = spinlock_lock_irqsave(&table->lock);
	id    = table->next_id;
	if (id < table->min_id || id > table->max_id || !id_table_index(table, id, &index)) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return false;
	}
	if (index == SIZE_MAX) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return false;
	}
	if (!id_table_grow_locked(table, index + 1u)) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return false;
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
	return true;
}

void* id_table_lookup(struct id_table* table, id_table_id_t id) {
	struct irq_state state;
	size_t           index;
	void*            object = NULL;

	if (table == NULL || !id_table_index(table, id, &index)) return NULL;

	state = spinlock_lock_irqsave(&table->lock);
	if (index < table->capacity) object = table->slots[index];
	spinlock_unlock_irqrestore(&table->lock, state);
	return object;
}

bool id_table_remove(struct id_table* table, id_table_id_t id, void** out_object) {
	struct irq_state state;
	size_t           index;
	void*            object;

	if (out_object != NULL) *out_object = NULL;
	if (table == NULL || !id_table_index(table, id, &index)) return false;

	state = spinlock_lock_irqsave(&table->lock);
	if (index >= table->capacity || table->slots[index] == NULL) {
		spinlock_unlock_irqrestore(&table->lock, state);
		return false;
	}

	object              = table->slots[index];
	table->slots[index] = NULL;
	if (table->count != 0u) table->count--;
	spinlock_unlock_irqrestore(&table->lock, state);

	if (out_object != NULL) *out_object = object;
	return true;
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
