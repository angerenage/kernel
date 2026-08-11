#include <base/cap.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/capability_call.h>
#include <core/lock.h>
#include <core/pmm.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define CAP_ID_TABLE_MIN 1u
#define CAP_ID_TABLE_MAX UINT64_MAX
#define CAP_OBJECT_UNREGISTER_BATCH 16u

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
	if (!channel_retain(endpoint)) {
		free(object);
		return NULL;
	}

	object->cap_object_id   = CAP_OBJECT_ID_INVALID;
	object->object_id       = object_id;
	object->endpoint        = endpoint;
	object->handler         = handler;
	object->reference_count = 1u;

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

struct cap_object* cap_object_create(uint64_t object_id, struct channel* endpoint, bool* out_created) {
	struct irq_state   state;
	struct cap_object* object;

	if (out_created != NULL) *out_created = false;
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
		cap_object_release(object);
		return NULL;
	}

	state                       = spinlock_lock_irqsave(&cap_object_table.lock);
	object->cap_object_id       = id;
	struct cap_object* existing = cap_object_find_locked(endpoint, object_id, NULL);
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);
	if (existing != object) {
		(void)id_table_remove(&cap_object_table, id, NULL);
		cap_object_release(object);
		return existing;
	}
	if (out_created != NULL) *out_created = true;

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
		cap_object_release(object);
		return NULL;
	}

	state                       = spinlock_lock_irqsave(&cap_object_table.lock);
	object->cap_object_id       = id;
	struct cap_object* existing = cap_object_find_locked(NULL, object_id, handler);
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);
	if (existing != object) {
		(void)id_table_remove(&cap_object_table, id, NULL);
		cap_object_release(object);
		return existing;
	}

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

static bool cap_object_retain_callback(void* value, void* context) {
	struct cap_object* object = value;
	(void)context;

	(void)__atomic_add_fetch(&object->reference_count, 1u, __ATOMIC_RELAXED);
	return true;
}

struct cap_object* cap_object_acquire(cap_object_id_t id) {
	if (id == CAP_OBJECT_ID_INVALID) return NULL;
	return id_table_lookup_retain(&cap_object_table, (id_table_id_t)id, cap_object_retain_callback, NULL);
}

void cap_object_release(struct cap_object* object) {
	uint64_t remaining;

	if (object == NULL) return;
	remaining = __atomic_sub_fetch(&object->reference_count, 1u, __ATOMIC_ACQ_REL);
	if (remaining != 0u) return;
	channel_release(object->endpoint);
	free(object);
}

bool cap_object_destroy_with_id(cap_object_id_t id) {
	if (id == CAP_OBJECT_ID_INVALID) return false;

	struct cap_object* object = NULL;
	if (id_table_remove(&cap_object_table, (id_table_id_t)id, (void**)&object) != ID_TABLE_OK) return false;

	cap_object_release(object);
	return true;
}

bool cap_object_destroy(struct cap_object* object) {
	if (object == NULL) return false;

	struct cap_object* removed = NULL;
	if (id_table_remove(&cap_object_table, object->cap_object_id, (void**)&removed) != ID_TABLE_OK) return false;

	if (removed != NULL) cap_object_release(removed);
	return true;
}

void cap_object_unregister_endpoint(struct channel* endpoint) {
	cap_object_id_t ids[CAP_OBJECT_UNREGISTER_BATCH];
	size_t          count;

	if (endpoint == NULL) return;
	do {
		struct irq_state state = spinlock_lock_irqsave(&cap_object_table.lock);
		count                  = 0u;
		for (size_t i = 0u; i < cap_object_table.capacity && count < CAP_OBJECT_UNREGISTER_BATCH; i++) {
			struct cap_object* object = cap_object_table.slots[i];
			if (object == NULL || object->endpoint != endpoint) continue;
			ids[count++] = object->cap_object_id;
		}
		spinlock_unlock_irqrestore(&cap_object_table.lock, state);

		for (size_t i = 0u; i < count; i++) (void)cap_object_destroy_with_id(ids[i]);
	} while (count != 0u);
}

static struct capability* capability_create_locked(cap_object_id_t cap_object_id, process_id_t target,
                                                   cap_rights_t rights, struct capability* parent) {
	struct capability* capability = malloc(sizeof(*capability));
	if (capability == NULL) return NULL;
	if (parent != NULL) (void)__atomic_add_fetch(&parent->reference_count, 1u, __ATOMIC_RELAXED);

	capability->cap_id          = CAP_ID_INVALID;
	capability->cap_object_id   = cap_object_id;
	capability->target          = target;
	capability->rights          = rights;
	capability->parent          = parent;
	capability->revoked         = false;
	capability->reference_count = 1u;

	return capability;
}

void cap_release(struct capability* capability) {
	struct capability* parent;

	if (capability == NULL) return;
	if (__atomic_sub_fetch(&capability->reference_count, 1u, __ATOMIC_ACQ_REL) != 0u) return;
	parent = capability->parent;
	free(capability);
	cap_release(parent);
}

static bool capability_retain_callback(void* value, void* context) {
	struct capability* capability = value;
	uint64_t           current;

	(void)context;
	current = __atomic_load_n(&capability->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&capability->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
}

struct capability* cap_acquire(cap_id_t id) {
	if (id == CAP_ID_INVALID) return NULL;
	return id_table_lookup_retain(&capability_table, (id_table_id_t)id, capability_retain_callback, NULL);
}

cap_rights_t cap_rights(const struct capability* capability) {
	return capability == NULL ? 0u : __atomic_load_n(&capability->rights, __ATOMIC_ACQUIRE);
}

bool cap_is_revoked(const struct capability* capability) {
	return capability == NULL || __atomic_load_n(&capability->revoked, __ATOMIC_ACQUIRE);
}

void cap_mark_revoked(struct capability* capability) {
	if (capability != NULL) __atomic_store_n(&capability->revoked, true, __ATOMIC_RELEASE);
}

bool cap_remove_rights(struct capability* capability, cap_rights_t rights) {
	cap_rights_t current;

	if (capability == NULL || rights == 0u) return false;
	current = cap_rights(capability);
	for (;;) {
		if ((rights & ~current) != 0u) return false;
		if (__atomic_compare_exchange_n(
				&capability->rights, &current, current & ~rights, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
}

static struct capability* capability_find_locked(cap_object_id_t cap_object_id, process_id_t target,
                                                 cap_rights_t rights, struct capability* parent) {
	for (size_t i = 0; i < capability_table.capacity; i++) {
		if (capability_table.slots[i] != NULL) {
			struct capability* cap = (struct capability*)capability_table.slots[i];
			if (cap->cap_object_id != cap_object_id) continue;
			if (cap->target != target) continue;
			if (cap_is_revoked(cap) || cap_rights(cap) != rights) continue;
			if (cap->parent != parent) continue;
			return cap;
		}
	}
	return NULL;
}

struct capability* cap_create(cap_object_id_t cap_object_id, process_id_t target, cap_rights_t rights,
                              struct capability* parent, bool* out_created) {
	struct irq_state   state;
	struct capability* capability;
	struct cap_object* object;

	if (out_created != NULL) *out_created = false;
	if (cap_object_id == CAP_OBJECT_ID_INVALID) return NULL;
	object = cap_object_acquire(cap_object_id);
	if (object == NULL) return NULL;
	if (parent != NULL && cap_is_valid(parent) != CAP_OK) {
		cap_object_release(object);
		return NULL;
	}

	state      = spinlock_lock_irqsave(&capability_table.lock);
	capability = capability_find_locked(cap_object_id, target, rights, parent);
	if (capability != NULL) {
		spinlock_unlock_irqrestore(&capability_table.lock, state);
		cap_object_release(object);
		return capability;
	}
	spinlock_unlock_irqrestore(&capability_table.lock, state);

	capability = capability_create_locked(cap_object_id, target, rights, parent);
	if (capability == NULL) {
		cap_object_release(object);
		return NULL;
	}

	id_table_id_t id;
	if (id_table_alloc(&capability_table, capability, &id) != ID_TABLE_OK) {
		cap_release(capability);
		cap_object_release(object);
		return NULL;
	}

	state                       = spinlock_lock_irqsave(&capability_table.lock);
	capability->cap_id          = (cap_id_t)id;
	struct capability* existing = capability_find_locked(cap_object_id, target, rights, parent);
	spinlock_unlock_irqrestore(&capability_table.lock, state);
	if (existing != capability) {
		(void)id_table_remove(&capability_table, id, NULL);
		cap_release(capability);
		cap_object_release(object);
		return existing;
	}
	struct cap_object* registered_object = cap_object_acquire(cap_object_id);
	if (registered_object == NULL || (parent != NULL && cap_is_valid(parent) != CAP_OK)) {
		(void)cap_destroy_by_id(capability->cap_id);
		cap_object_release(registered_object);
		cap_object_release(object);
		return NULL;
	}
	cap_object_release(registered_object);
	cap_object_release(object);
	if (out_created != NULL) *out_created = true;

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

	if (removed != NULL) {
		cap_mark_revoked(removed);
		cap_release(removed);
	}
	return true;
}

bool cap_destroy_by_id(cap_id_t id) {
	if (id == CAP_ID_INVALID) return false;

	struct capability* removed = NULL;
	if (id_table_remove(&capability_table, (id_table_id_t)id, (void**)&removed) != ID_TABLE_OK) return false;

	if (removed != NULL) {
		cap_mark_revoked(removed);
		cap_release(removed);
	}
	return true;
}

void cap_revoke_for_process(process_id_t target) {
	if (target == PROCESS_PID_INVALID) return;

	struct irq_state state = spinlock_lock_irqsave(&capability_table.lock);
	for (size_t i = 0; i < capability_table.capacity; i++) {
		struct capability* cap = (struct capability*)capability_table.slots[i];
		if (cap != NULL && cap->target == target) cap_mark_revoked(cap);
	}
	spinlock_unlock_irqrestore(&capability_table.lock, state);
}

bool cap_object_alive(struct capability* cap) {
	struct cap_object* object;

	if (cap == NULL) return false;
	object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL) return false;
	cap_object_release(object);
	return true;
}

enum cap_result cap_is_authorized(process_id_t caller, struct capability* cap) {
	if (cap == NULL || caller == PROCESS_PID_INVALID) return CAP_INVALID_ARGUMENTS;

	if (cap_is_revoked(cap)) return CAP_REVOKED;

	struct cap_object* object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL) return CAP_OBJECT_DESTROYED;

	if (cap->target == caller) {
		cap_object_release(object);
		return CAP_OK;
	}

	if (object->endpoint != NULL && object->endpoint->owner_pid == caller) {
		cap_object_release(object);
		return CAP_OK;
	}
	cap_object_release(object);

	struct capability* parent = cap->parent;
	while (parent) {
		if (cap_is_revoked(parent)) return CAP_REVOKED;
		struct cap_object* parent_object = cap_object_acquire(parent->cap_object_id);
		if (parent_object == NULL) return CAP_OBJECT_DESTROYED;
		cap_object_release(parent_object);
		if (parent->target == caller) return CAP_OK;
		parent = parent->parent;
	}

	return CAP_NOT_AUTHORIZED;
}

enum cap_result cap_is_valid(struct capability* cap) {
	if (cap == NULL) return CAP_INVALID_ARGUMENTS;

	struct capability* current = cap;
	while (current) {
		if (cap_is_revoked(current)) return CAP_REVOKED;
		struct cap_object* object = cap_object_acquire(current->cap_object_id);
		if (object == NULL) return CAP_OBJECT_DESTROYED;
		cap_object_release(object);
		current = current->parent;
	}

	return CAP_OK;
}

void capability_init(void) {
	id_table_init(&cap_object_table, "cap_object_table", CAP_ID_TABLE_MIN, CAP_ID_TABLE_MAX);
	id_table_init(&capability_table, "capability_table", CAP_ID_TABLE_MIN, CAP_ID_TABLE_MAX);
	cap_pending_call_init();
}

size_t capability_object_count(void) {
	return id_table_count(&cap_object_table);
}

size_t capability_count(void) {
	return id_table_count(&capability_table);
}
