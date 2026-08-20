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

static struct spinlock capability_topology_lock =
	SPINLOCK_INIT_CLASS("capability_topology", SPINLOCK_ORDER_CAPABILITY, SPINLOCK_FLAG_IRQSAVE);

static struct capability* capability_subtree_next_locked(struct capability* root, struct capability* current) {
	if (root == NULL || current == NULL) return NULL;
	if (current->first_child != NULL) return current->first_child;

	while (current != root) {
		if (current->next_sibling != NULL) return current->next_sibling;
		current = current->parent;
		if (current == NULL) return NULL;
	}
	return NULL;
}

static void capability_set_subtree_removed_locked(struct capability* root, bool removed) {
	struct capability* current = root;

	while (current != NULL) {
		__atomic_store_n(&current->removed, removed, __ATOMIC_RELEASE);
		current = capability_subtree_next_locked(root, current);
	}
}

static void capability_remove_subtree_rights_locked(struct capability* root, cap_rights_t rights) {
	struct capability* current = root;

	while (current != NULL) {
		(void)__atomic_fetch_and(&current->rights, ~rights, __ATOMIC_ACQ_REL);
		current = capability_subtree_next_locked(root, current);
	}
}

static struct capability* capability_subtree_first_leaf_locked(struct capability* root) {
	struct capability* current = root;

	while (current != NULL && current->first_child != NULL) current = current->first_child;
	return current;
}

static struct capability* capability_subtree_postorder_next_locked(struct capability* root,
                                                                   struct capability* current) {
	struct capability* next;

	if (root == NULL || current == NULL || current == root) return NULL;
	if (current->next_sibling == NULL) return current->parent;

	next = current->next_sibling;
	while (next->first_child != NULL) next = next->first_child;
	return next;
}

static void capability_attach_locked(struct capability* capability) {
	struct capability* parent;

	if (capability == NULL || capability->parent == NULL) return;
	parent                   = capability->parent;
	capability->next_sibling = parent->first_child;
	parent->first_child      = capability;
}

static bool capability_detach_locked(struct capability* capability) {
	struct capability** link;
	struct capability*  parent;

	if (capability == NULL) return false;
	parent = capability->parent;
	if (parent == NULL) {
		capability->next_sibling = NULL;
		return true;
	}

	link = &parent->first_child;
	while (*link != NULL && *link != capability) link = &(*link)->next_sibling;
	if (*link != capability) return false;

	*link                    = capability->next_sibling;
	capability->parent       = NULL;
	capability->next_sibling = NULL;
	return true;
}

static bool capability_is_published_locked(struct capability* capability) {
	if (capability == NULL || capability->cap_id == CAP_ID_INVALID) return false;
	if (__atomic_load_n(&capability->removed, __ATOMIC_ACQUIRE)) return false;
	return id_table_lookup(&capability_table, (id_table_id_t)capability->cap_id) == capability;
}

static bool capability_remove_subtree_locked(struct capability* root) {
	struct capability* current;
	bool               success = true;

	if (!capability_is_published_locked(root)) return false;

	capability_set_subtree_removed_locked(root, true);
	if (!capability_detach_locked(root)) {
		capability_set_subtree_removed_locked(root, false);
		return false;
	}

	current = capability_subtree_first_leaf_locked(root);
	while (current != NULL) {
		struct capability* next    = capability_subtree_postorder_next_locked(root, current);
		struct capability* removed = NULL;

		if (id_table_remove(&capability_table, (id_table_id_t)current->cap_id, (void**)&removed) != ID_TABLE_OK ||
		    removed != current) {
			success = false;
		}
		else {
			current->parent       = NULL;
			current->first_child  = NULL;
			current->next_sibling = NULL;
			cap_release(removed);
		}
		current = next;
	}

	return success;
}

static void capability_remove_object_subtrees_locked(cap_object_id_t object_id) {
	size_t cursor = 0u;

	for (;;) {
		struct capability* candidate   = NULL;
		struct irq_state   table_state = spinlock_lock_irqsave(&capability_table.lock);

		for (; cursor < capability_table.capacity; cursor++) {
			struct capability* capability = capability_table.slots[cursor];
			if (capability == NULL || capability->cap_object_id != object_id) continue;
			candidate = capability;
			cursor++;
			break;
		}
		spinlock_unlock_irqrestore(&capability_table.lock, table_state);
		if (candidate == NULL) return;
		(void)capability_remove_subtree_locked(candidate);
	}
}

static struct cap_object* cap_object_create_locked(uint64_t object_id, struct channel* endpoint,
                                                   cap_kernel_handler_t         handler,
                                                   cap_kernel_process_cleanup_t process_cleanup,
                                                   cap_kernel_destroy_t         destroy) {
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
	object->process_cleanup = process_cleanup;
	object->destroy         = destroy;
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

cap_object_id_t cap_object_create(uint64_t object_id, struct channel* endpoint, bool* out_created) {
	struct irq_state   state;
	struct cap_object* object;

	if (out_created != NULL) *out_created = false;
	state  = spinlock_lock_irqsave(&cap_object_table.lock);
	object = cap_object_find_locked(endpoint, object_id, NULL);
	if (object != NULL) {
		cap_object_id_t id = object->cap_object_id;
		spinlock_unlock_irqrestore(&cap_object_table.lock, state);
		return id;
	}
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);

	object = cap_object_create_locked(object_id, endpoint, NULL, NULL, NULL);
	if (object == NULL) return CAP_OBJECT_ID_INVALID;
	/* Keep a creator reference while the freshly published table entry can already be removed by another CPU. */
	(void)__atomic_add_fetch(&object->reference_count, 1u, __ATOMIC_RELAXED);

	id_table_id_t id;
	if (id_table_alloc(&cap_object_table, object, &id) != ID_TABLE_OK) {
		cap_object_release(object);
		cap_object_release(object);
		return CAP_OBJECT_ID_INVALID;
	}

	state                        = spinlock_lock_irqsave(&cap_object_table.lock);
	object->cap_object_id        = id;
	struct cap_object* existing  = cap_object_find_locked(endpoint, object_id, NULL);
	cap_object_id_t    result_id = existing == NULL ? CAP_OBJECT_ID_INVALID : existing->cap_object_id;
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);
	if (existing != object) {
		struct cap_object* removed = NULL;
		if (id_table_remove(&cap_object_table, id, (void**)&removed) == ID_TABLE_OK) cap_object_release(removed);
		cap_object_release(object);
		return result_id;
	}
	if (out_created != NULL) *out_created = true;

	cap_object_release(object);
	return result_id;
}

cap_object_id_t cap_object_create_kernel(uint64_t object_id, cap_kernel_handler_t handler, bool* out_created) {
	return cap_object_create_kernel_managed(object_id, handler, NULL, NULL, out_created);
}

cap_object_id_t cap_object_create_kernel_managed(uint64_t object_id, cap_kernel_handler_t handler,
                                                 cap_kernel_process_cleanup_t process_cleanup,
                                                 cap_kernel_destroy_t destroy, bool* out_created) {
	struct irq_state   state;
	struct cap_object* object;

	if (out_created != NULL) *out_created = false;
	if (handler == NULL) return CAP_OBJECT_ID_INVALID;

	state  = spinlock_lock_irqsave(&cap_object_table.lock);
	object = cap_object_find_locked(NULL, object_id, handler);
	if (object != NULL) {
		cap_object_id_t id = object->cap_object_id;
		spinlock_unlock_irqrestore(&cap_object_table.lock, state);
		return id;
	}
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);

	object = cap_object_create_locked(object_id, NULL, handler, process_cleanup, destroy);
	if (object == NULL) return CAP_OBJECT_ID_INVALID;
	(void)__atomic_add_fetch(&object->reference_count, 1u, __ATOMIC_RELAXED);

	id_table_id_t id;
	if (id_table_alloc(&cap_object_table, object, &id) != ID_TABLE_OK) {
		cap_object_release(object);
		cap_object_release(object);
		return CAP_OBJECT_ID_INVALID;
	}

	state                        = spinlock_lock_irqsave(&cap_object_table.lock);
	object->cap_object_id        = id;
	struct cap_object* existing  = cap_object_find_locked(NULL, object_id, handler);
	cap_object_id_t    result_id = existing == NULL ? CAP_OBJECT_ID_INVALID : existing->cap_object_id;
	spinlock_unlock_irqrestore(&cap_object_table.lock, state);
	if (existing != object) {
		struct cap_object* removed = NULL;
		if (id_table_remove(&cap_object_table, id, (void**)&removed) == ID_TABLE_OK) cap_object_release(removed);
		cap_object_release(object);
		return result_id;
	}
	if (out_created != NULL) *out_created = true;

	cap_object_release(object);
	return result_id;
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
	if (object->destroy != NULL) object->destroy(object->object_id);
	channel_release(object->endpoint);
	free(object);
}

bool cap_object_destroy_with_id(cap_object_id_t id) {
	struct irq_state   topology_state;
	struct cap_object* object = NULL;

	if (id == CAP_OBJECT_ID_INVALID) return false;

	topology_state = spinlock_lock_irqsave(&capability_topology_lock);
	if (id_table_remove(&cap_object_table, (id_table_id_t)id, (void**)&object) != ID_TABLE_OK) {
		spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
		return false;
	}
	capability_remove_object_subtrees_locked(id);
	spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);

	cap_object_release(object);
	return true;
}

bool cap_object_destroy(struct cap_object* object) {
	if (object == NULL) return false;
	return cap_object_destroy_with_id(object->cap_object_id);
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

void cap_object_cleanup_for_process(process_id_t process) {
	if (process == PROCESS_PID_INVALID) return;
	for (;;) {
		size_t cursor        = 0u;
		bool   destroyed_any = false;

		for (;;) {
			struct cap_object* object = NULL;
			struct irq_state   state  = spinlock_lock_irqsave(&cap_object_table.lock);
			for (; cursor < cap_object_table.capacity; cursor++) {
				struct cap_object* candidate = cap_object_table.slots[cursor];
				if (candidate == NULL || candidate->process_cleanup == NULL) continue;
				(void)__atomic_add_fetch(&candidate->reference_count, 1u, __ATOMIC_RELAXED);
				object = candidate;
				cursor++;
				break;
			}
			spinlock_unlock_irqrestore(&cap_object_table.lock, state);
			if (object == NULL) break;

			cap_object_id_t id      = object->cap_object_id;
			bool            destroy = object->process_cleanup(object->object_id, process);
			cap_object_release(object);
			if (destroy && cap_object_destroy_with_id(id)) destroyed_any = true;
		}
		if (!destroyed_any) return;
	}
}

static struct capability* capability_create_locked(cap_object_id_t cap_object_id, process_id_t target,
                                                   cap_rights_t rights, struct capability* parent) {
	struct capability* capability = malloc(sizeof(*capability));
	if (capability == NULL) return NULL;

	capability->cap_id          = CAP_ID_INVALID;
	capability->cap_object_id   = cap_object_id;
	capability->target          = target;
	capability->rights          = rights;
	capability->parent          = parent;
	capability->first_child     = NULL;
	capability->next_sibling    = NULL;
	capability->removed         = true;
	capability->reference_count = 1u;

	return capability;
}

void cap_release(struct capability* capability) {
	if (capability == NULL) return;
	if (__atomic_sub_fetch(&capability->reference_count, 1u, __ATOMIC_ACQ_REL) != 0u) return;
	free(capability);
}

static bool capability_retain_callback(void* value, void* context) {
	struct capability* capability = value;
	uint64_t           current;

	(void)context;
	if (__atomic_load_n(&capability->removed, __ATOMIC_ACQUIRE)) return false;
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

bool cap_is_removed(const struct capability* capability) {
	return capability == NULL || __atomic_load_n(&capability->removed, __ATOMIC_ACQUIRE);
}

bool cap_remove_rights(struct capability* capability, cap_rights_t rights) {
	struct irq_state state;
	cap_rights_t     current;

	if (capability == NULL || rights == 0u) return false;
	state   = spinlock_lock_irqsave(&capability_topology_lock);
	current = cap_rights(capability);
	if (cap_is_removed(capability) || (rights & ~current) != 0u) {
		spinlock_unlock_irqrestore(&capability_topology_lock, state);
		return false;
	}
	capability_remove_subtree_rights_locked(capability, rights);
	spinlock_unlock_irqrestore(&capability_topology_lock, state);
	return true;
}

static struct capability* capability_find_locked(cap_object_id_t cap_object_id, process_id_t target,
                                                 cap_rights_t rights, struct capability* parent) {
	for (size_t i = 0; i < capability_table.capacity; i++) {
		if (capability_table.slots[i] != NULL) {
			struct capability* cap = (struct capability*)capability_table.slots[i];
			if (cap->cap_object_id != cap_object_id) continue;
			if (cap->target != target) continue;
			if (cap_is_removed(cap) || cap_rights(cap) != rights) continue;
			if (cap->parent != parent) continue;
			return cap;
		}
	}
	return NULL;
}

cap_id_t cap_create(cap_object_id_t cap_object_id, process_id_t target, cap_rights_t rights, struct capability* parent,
                    bool* out_created) {
	struct irq_state   topology_state;
	struct irq_state   table_state;
	struct capability* capability;
	struct cap_object* object;
	struct capability* existing;
	id_table_id_t      id;
	cap_id_t           result_id;

	if (out_created != NULL) *out_created = false;
	if (cap_object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
	object = cap_object_acquire(cap_object_id);
	if (object == NULL) return CAP_ID_INVALID;

	capability = capability_create_locked(cap_object_id, target, rights, parent);
	if (capability == NULL) {
		cap_object_release(object);
		return CAP_ID_INVALID;
	}

	topology_state = spinlock_lock_irqsave(&capability_topology_lock);
	if (id_table_lookup(&cap_object_table, (id_table_id_t)cap_object_id) == NULL ||
	    (parent != NULL && (!capability_is_published_locked(parent) || (rights & ~cap_rights(parent)) != 0u))) {
		spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
		cap_release(capability);
		cap_object_release(object);
		return CAP_ID_INVALID;
	}

	table_state = spinlock_lock_irqsave(&capability_table.lock);
	existing    = capability_find_locked(cap_object_id, target, rights, parent);
	result_id   = existing == NULL ? CAP_ID_INVALID : existing->cap_id;
	spinlock_unlock_irqrestore(&capability_table.lock, table_state);
	if (existing != NULL) {
		spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
		cap_release(capability);
		cap_object_release(object);
		return result_id;
	}

	if (id_table_alloc(&capability_table, capability, &id) != ID_TABLE_OK) {
		spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
		cap_release(capability);
		cap_object_release(object);
		return CAP_ID_INVALID;
	}

	capability->cap_id = (cap_id_t)id;
	capability_attach_locked(capability);
	__atomic_store_n(&capability->removed, false, __ATOMIC_RELEASE);
	result_id = capability->cap_id;
	spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);

	cap_object_release(object);
	if (out_created != NULL) *out_created = true;
	return result_id;
}

struct capability* cap_lookup(cap_id_t id) {
	if (id == CAP_ID_INVALID) return NULL;
	return (struct capability*)id_table_lookup(&capability_table, (id_table_id_t)id);
}

bool cap_destroy(struct capability* capability) {
	struct irq_state topology_state;
	bool             removed;

	if (capability == NULL) return false;

	topology_state = spinlock_lock_irqsave(&capability_topology_lock);
	removed        = capability_remove_subtree_locked(capability);
	spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
	return removed;
}

bool cap_destroy_by_id(cap_id_t id) {
	struct capability* capability;
	bool               destroyed;

	if (id == CAP_ID_INVALID) return false;
	capability = cap_acquire(id);
	if (capability == NULL) return false;
	destroyed = cap_destroy(capability);
	cap_release(capability);
	return destroyed;
}

void cap_revoke_for_process(process_id_t target) {
	struct irq_state topology_state;
	size_t           cursor = 0u;

	if (target == PROCESS_PID_INVALID) return;

	topology_state = spinlock_lock_irqsave(&capability_topology_lock);
	for (;;) {
		struct capability* candidate   = NULL;
		struct irq_state   table_state = spinlock_lock_irqsave(&capability_table.lock);

		for (; cursor < capability_table.capacity; cursor++) {
			struct capability* cap = capability_table.slots[cursor];
			if (cap == NULL || cap->target != target) continue;
			candidate = cap;
			cursor++;
			break;
		}
		spinlock_unlock_irqrestore(&capability_table.lock, table_state);
		if (candidate == NULL) break;
		(void)capability_remove_subtree_locked(candidate);
	}
	spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
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
	struct irq_state   topology_state;
	struct cap_object* object;
	struct capability* parent;
	enum cap_result    result = CAP_NOT_AUTHORIZED;

	if (cap == NULL || caller == PROCESS_PID_INVALID) return CAP_INVALID_ARGUMENTS;

	topology_state = spinlock_lock_irqsave(&capability_topology_lock);
	if (cap_is_removed(cap)) {
		result = CAP_NOT_FOUND;
		goto out;
	}
	if (cap->target == caller) {
		result = CAP_OK;
		goto out;
	}

	object = id_table_lookup(&cap_object_table, (id_table_id_t)cap->cap_object_id);
	if (object == NULL) {
		result = CAP_OBJECT_DESTROYED;
		goto out;
	}
	if (object->endpoint != NULL && object->endpoint->owner_pid == caller) {
		result = CAP_OK;
		goto out;
	}

	parent = cap->parent;
	while (parent != NULL) {
		if (parent->target == caller) {
			result = CAP_OK;
			goto out;
		}
		parent = parent->parent;
	}

out:
	spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
	return result;
}

enum cap_result cap_is_valid(struct capability* cap) {
	struct irq_state topology_state;
	enum cap_result  result;

	if (cap == NULL) return CAP_INVALID_ARGUMENTS;

	topology_state = spinlock_lock_irqsave(&capability_topology_lock);
	result         = cap_is_removed(cap) ? CAP_NOT_FOUND : CAP_OK;
	spinlock_unlock_irqrestore(&capability_topology_lock, topology_state);
	return result;
}

void capability_init(void) {
	spinlock_init_class(
		&capability_topology_lock, "capability_topology", SPINLOCK_ORDER_CAPABILITY, SPINLOCK_FLAG_IRQSAVE);
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
