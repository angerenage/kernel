#include <base/cap.h>
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

static struct cap_object* cap_object_create_locked(uint64_t object_id, struct channel* endpoint) {
	struct cap_object* object = malloc(sizeof(*object));
	if (object == NULL) return NULL;

	object->cap_object_id = 0;
	object->object_id     = object_id;
	object->endpoint      = endpoint;

	return object;
}

struct cap_object* cap_object_create(uint64_t object_id, struct channel* endpoint) {
	struct cap_object* object = cap_object_create_locked(object_id, endpoint);
	if (object == NULL) return NULL;

	id_table_id_t id;
	if (id_table_alloc(&cap_object_table, object, &id) != ID_TABLE_OK) {
		free(object);
		return NULL;
	}

	object->cap_object_id = id;
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

bool cap_object_destroy(struct cap_object* object) {
	if (object == NULL) return false;

	bool result = id_table_remove(&cap_object_table, object->cap_object_id, NULL) == ID_TABLE_OK;
	if (result) free(object);
	return result;
}

static struct capability* capability_create_locked(struct cap_object* object, process_id_t target, cap_rights_t rights,
                                                   struct capability* parent) {
	struct capability* capability = malloc(sizeof(*capability));
	if (capability == NULL) return NULL;

	capability->cap_id  = CAP_ID_INVALID;
	capability->object  = object;
	capability->target  = target;
	capability->rights  = rights;
	capability->parent  = parent;
	capability->revoked = false;

	return capability;
}

struct capability* cap_create(struct cap_object* object, process_id_t target, cap_rights_t rights,
                              struct capability* parent) {
	if (object == NULL) return NULL;

	struct capability* capability = capability_create_locked(object, target, rights, parent);
	if (capability == NULL) return NULL;

	id_table_id_t id;
	if (id_table_alloc(&capability_table, capability, &id) != ID_TABLE_OK) {
		free(capability);
		return NULL;
	}

	capability->cap_id = (cap_id_t)id;
	return capability;
}

struct capability* cap_lookup(cap_id_t id) {
	if (id == CAP_ID_INVALID) return NULL;
	return (struct capability*)id_table_lookup(&capability_table, (id_table_id_t)id);
}

bool cap_destroy(struct capability* capability) {
	if (capability == NULL) return false;

	bool result = id_table_remove(&capability_table, (id_table_id_t)capability->cap_id, NULL) == ID_TABLE_OK;
	if (result) free(capability);
	return result;
}

enum cap_result cap_is_authorized(process_id_t caller, struct capability* cap) {
	if (cap == NULL || caller == PROCESS_PID_INVALID) return CAP_INVALID_ARGUMENTS;

	if (cap->revoked) return CAP_REVOKED;

	if (cap->target == caller) return CAP_OK;

	if (cap->object->endpoint != NULL && cap->object->endpoint->owner_pid == caller) return CAP_OK;

	struct capability* parent = cap->parent;
	while (parent) {
		if (parent->revoked) return CAP_REVOKED;
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
