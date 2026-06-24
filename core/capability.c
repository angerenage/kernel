#include <base/cap.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/lock.h>
#include <core/pmm.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define CAP_ID_TABLE_MIN 1u
#define CAP_ID_TABLE_MAX UINT64_MAX

static struct id_table cap_object_table = {
	.lock    = SPINLOCK_INIT_CLASS("cap_object_table", SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),
	.next_id = CAP_ID_TABLE_MIN,
	.min_id  = CAP_ID_TABLE_MIN,
	.max_id  = CAP_ID_TABLE_MAX,
};

static struct id_table capability_table = {
	.lock    = SPINLOCK_INIT_CLASS("capability_table", SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),
	.next_id = CAP_ID_TABLE_MIN,
	.min_id  = CAP_ID_TABLE_MIN,
	.max_id  = CAP_ID_TABLE_MAX,
};

static struct cap_object* cap_object_create_locked(uint64_t object_id, struct channel* endpoint,
                                                   cap_kernel_handler_t handler) {
	struct cap_object* object = malloc(sizeof(*object));
	if (object == NULL) return NULL;

	object->cap_object_id = ID_TABLE_ID_INVALID;
	object->object_id     = object_id;
	object->endpoint      = endpoint;
	object->handler       = handler;

	return object;
}

static struct cap_object* cap_object_find_locked(struct channel* endpoint, uint64_t object_id,
                                                 cap_kernel_handler_t handler) {
	for (size_t i = 0; i < cap_object_table.capacity; i++) {
		if (cap_object_table.slots[i] != NULL) {
			struct cap_object* obj = (struct cap_object*)cap_object_table.slots[i];
			if (obj->object_id != object_id) continue;
			if (obj->endpoint != endpoint) continue;
			if (handler != NULL && obj->handler != handler) continue;
			return obj;
		}
	}
	return NULL;
}

struct cap_object* cap_object_create(uint64_t object_id, struct channel* endpoint) {
	struct irq_state   state;
	struct cap_object* object;

	state  = spinlock_lock_irqsave(&cap_object_table.lock);
	object = cap_object_find_locked(endpoint, object_id, NULL);
	if (object != NULL) {
		spinlock_unlock_irqrestore(&cap_object_table.lock, state);
		return object;
	}
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);

	object = cap_object_create_locked(object_id, endpoint, NULL);
	if (object == NULL) return NULL;

	id_table_id_t id;
	if (id_table_alloc(&cap_object_table, object, &id) != ID_TABLE_OK) {
		free(object);
		return NULL;
	}

	state                 = spinlock_lock_irqsave(&cap_object_table.lock);
	object->cap_object_id = id;
	if (cap_object_find_locked(endpoint, object_id, NULL) != object) {
		(void)id_table_remove(&cap_object_table, id, NULL);
		struct cap_object* existing = cap_object_find_locked(endpoint, object_id, NULL);
		spinlock_unlock_irqrestore(&cap_object_table.lock, state);
		free(object);
		return existing;
	}
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);

	return object;
}

struct cap_object* cap_object_create_kernel(uint64_t object_id, cap_kernel_handler_t handler) {
	struct irq_state   state;
	struct cap_object* object;

	if (handler == NULL) return NULL;

	state  = spinlock_lock_irqsave(&cap_object_table.lock);
	object = cap_object_find_locked(NULL, object_id, handler);
	if (object != NULL) {
		spinlock_unlock_irqrestore(&cap_object_table.lock, state);
		return object;
	}
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);

	object = cap_object_create_locked(object_id, NULL, handler);
	if (object == NULL) return NULL;

	id_table_id_t id;
	if (id_table_alloc(&cap_object_table, object, &id) != ID_TABLE_OK) {
		free(object);
		return NULL;
	}

	state                 = spinlock_lock_irqsave(&cap_object_table.lock);
	object->cap_object_id = id;
	if (cap_object_find_locked(NULL, object_id, handler) != object) {
		(void)id_table_remove(&cap_object_table, id, NULL);
		struct cap_object* existing = cap_object_find_locked(NULL, object_id, handler);
		spinlock_unlock_irqrestore(&cap_object_table.lock, state);
		free(object);
		return existing;
	}
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);

	return object;
}

struct cap_object* cap_object_lookup(struct channel* endpoint, uint64_t object_id) {
	struct irq_state   state;
	struct cap_object* object = NULL;

	state = spinlock_lock_irqsave(&cap_object_table.lock);
	for (size_t i = 0; i < cap_object_table.capacity; i++) {
		if (cap_object_table.slots[i] != NULL) {
			struct cap_object* obj = (struct cap_object*)cap_object_table.slots[i];
			if (obj->object_id == object_id && obj->endpoint == endpoint) {
				object = obj;
				break;
			}
		}
	}
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);

	return object;
}

struct cap_object* cap_object_get(id_table_id_t id) {
	if (id == ID_TABLE_ID_INVALID) return NULL;
	return (struct cap_object*)id_table_lookup(&cap_object_table, id);
}

bool cap_object_destroy_with_id(id_table_id_t id) {
	if (id == ID_TABLE_ID_INVALID) return false;

	struct cap_object* object = NULL;
	if (id_table_remove(&cap_object_table, id, (void**)&object) != ID_TABLE_OK) return false;

	free(object);
	return true;
}

bool cap_object_destroy(struct cap_object* object) {
	if (object == NULL) return false;

	struct cap_object* removed = NULL;
	if (id_table_remove(&cap_object_table, object->cap_object_id, (void**)&removed) != ID_TABLE_OK) return false;

	if (removed != NULL) free(removed);
	return true;
}

static struct capability* capability_create_locked(id_table_id_t cap_object_id, process_id_t target,
                                                   cap_rights_t rights, struct capability* parent) {
	struct capability* capability = malloc(sizeof(*capability));
	if (capability == NULL) return NULL;

	capability->cap_id        = CAP_ID_INVALID;
	capability->cap_object_id = cap_object_id;
	capability->target        = target;
	capability->rights        = rights;
	capability->parent        = parent;
	capability->revoked       = false;

	return capability;
}

static struct capability* capability_find_locked(id_table_id_t cap_object_id, process_id_t target, cap_rights_t rights,
                                                 struct capability* parent) {
	for (size_t i = 0; i < capability_table.capacity; i++) {
		if (capability_table.slots[i] != NULL) {
			struct capability* cap = (struct capability*)capability_table.slots[i];
			if (cap->cap_object_id != cap_object_id) continue;
			if (cap->target != target) continue;
			if (cap->rights != rights) continue;
			if (cap->parent != parent) continue;
			return cap;
		}
	}
	return NULL;
}

struct capability* cap_create(id_table_id_t cap_object_id, process_id_t target, cap_rights_t rights,
                              struct capability* parent) {
	struct irq_state   state;
	struct capability* capability;

	if (cap_object_id == ID_TABLE_ID_INVALID) return NULL;

	state      = spinlock_lock_irqsave(&capability_table.lock);
	capability = capability_find_locked(cap_object_id, target, rights, parent);
	if (capability != NULL) {
		spinlock_unlock_irqrestore(&capability_table.lock, state);
		return capability;
	}
	spinlock_unlock_irqrestore(&capability_table.lock, state);

	capability = capability_create_locked(cap_object_id, target, rights, parent);
	if (capability == NULL) return NULL;

	id_table_id_t id;
	if (id_table_alloc(&capability_table, capability, &id) != ID_TABLE_OK) {
		free(capability);
		return NULL;
	}

	state              = spinlock_lock_irqsave(&capability_table.lock);
	capability->cap_id = (cap_id_t)id;
	if (capability_find_locked(cap_object_id, target, rights, parent) != capability) {
		(void)id_table_remove(&capability_table, id, NULL);
		struct capability* existing = capability_find_locked(cap_object_id, target, rights, parent);
		spinlock_unlock_irqrestore(&capability_table.lock, state);
		free(capability);
		return existing;
	}
	spinlock_unlock_irqrestore(&capability_table.lock, state);

	return capability;
}

struct capability* cap_lookup(cap_id_t id) {
	if (id == CAP_ID_INVALID) return NULL;
	return (struct capability*)id_table_lookup(&capability_table, (id_table_id_t)id);
}

bool cap_destroy(struct capability* capability) {
	if (capability == NULL) return false;

	struct capability* removed = NULL;
	if (id_table_remove(&capability_table, (id_table_id_t)capability->cap_id, (void**)&removed) != ID_TABLE_OK) {
		return false;
	}

	if (removed != NULL) free(removed);
	return true;
}

bool cap_destroy_by_id(cap_id_t id) {
	if (id == CAP_ID_INVALID) return false;

	struct capability* removed = NULL;
	if (id_table_remove(&capability_table, (id_table_id_t)id, (void**)&removed) != ID_TABLE_OK) return false;

	if (removed != NULL) free(removed);
	return true;
}

void cap_revoke_for_process(process_id_t target) {
	if (target == PROCESS_PID_INVALID) return;

	struct irq_state state = spinlock_lock_irqsave(&capability_table.lock);

	for (size_t i = 0; i < capability_table.capacity; i++) {
		struct capability* cap = (struct capability*)capability_table.slots[i];
		if (cap == NULL) continue;
		if (cap->target == target) cap->revoked = true;
	}

	spinlock_unlock_irqrestore(&capability_table.lock, state);
}

bool cap_object_alive(struct capability* cap) {
	if (cap == NULL) return false;
	return cap_object_get(cap->cap_object_id) != NULL;
}

enum cap_result cap_is_authorized(process_id_t caller, struct capability* cap) {
	if (cap == NULL || caller == PROCESS_PID_INVALID) return CAP_INVALID_ARGUMENTS;

	if (cap->revoked) return CAP_REVOKED;

	struct cap_object* object = cap_object_get(cap->cap_object_id);
	if (object == NULL) return CAP_OBJECT_DESTROYED;

	if (cap->target == caller) return CAP_OK;

	if (object->endpoint != NULL && object->endpoint->owner_pid == caller) return CAP_OK;

	struct capability* parent = cap->parent;
	while (parent) {
		if (parent->revoked) return CAP_REVOKED;
		struct cap_object* parent_object = cap_object_get(parent->cap_object_id);
		if (parent_object == NULL) return CAP_OBJECT_DESTROYED;
		if (parent->target == caller) return CAP_OK;
		parent = parent->parent;
	}

	return CAP_NOT_AUTHORIZED;
}

enum cap_result cap_is_valid(struct capability* cap) {
	if (cap == NULL) return CAP_INVALID_ARGUMENTS;

	struct capability* current = cap;
	while (current) {
		if (current->revoked) return CAP_REVOKED;
		if (cap_object_get(current->cap_object_id) == NULL) return CAP_OBJECT_DESTROYED;
		current = current->parent;
	}

	return CAP_OK;
}

void capability_init(void) {
	id_table_init(&cap_object_table, "cap_object_table", CAP_ID_TABLE_MIN, CAP_ID_TABLE_MAX);
	id_table_init(&capability_table, "capability_table", CAP_ID_TABLE_MIN, CAP_ID_TABLE_MAX);
}

size_t capability_object_count(void) {
	return id_table_count(&cap_object_table);
}

size_t capability_count(void) {
	return id_table_count(&capability_table);
}
