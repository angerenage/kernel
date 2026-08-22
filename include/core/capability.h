#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/channel.h>
#include <core/id_table.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Handler invoked by sys_cap_call for kernel-owned capabilities. */
typedef syscall_result_t (*cap_kernel_handler_t)(const struct cap_request* req);
/* Handler invoked before a process resource context disappears. */
typedef bool (*cap_kernel_process_cleanup_t)(uint64_t object_id, process_id_t process);
/* Handler invoked when managed kernel routing metadata is finalized. */
typedef void (*cap_kernel_destroy_t)(uint64_t object_id);

/* Lifecycle events observed by a kernel capability provider. */
enum cap_object_event {
	CAP_OBJECT_EVENT_ZERO_GRANTS = 1u,
};

struct cap_object;

/* Handler invoked for an eligible kernel capability-object lifecycle event. */
typedef void (*cap_object_event_handler_t)(struct cap_object* object, enum cap_object_event event);

/* Stable identifier for a registered cap_object. */
typedef id_table_id_t cap_object_id_t;

#define CAP_OBJECT_ID_INVALID ID_TABLE_ID_INVALID

/* A kernel or userspace object that can be referenced by capabilities. */
struct cap_object {
	cap_object_id_t              cap_object_id;
	uint64_t                     object_id;
	struct channel*              endpoint;
	cap_kernel_handler_t         handler;
	cap_kernel_process_cleanup_t process_cleanup;
	cap_kernel_destroy_t         destroy;
	cap_object_event_handler_t   event_handler;
	struct cap_object*           event_next;
	uint64_t                     reference_count;
	size_t                       grant_count;
	size_t                       active_calls;
	bool                         event_pending;
};

/* A capability grants a target process rights on a cap_object. Capabilities form a delegation tree through parent. */
struct capability {
	cap_id_t           cap_id;
	cap_object_id_t    cap_object_id;
	process_id_t       target;
	cap_rights_t       rights;
	struct capability* parent;
	struct capability* first_child;
	struct capability* next_sibling;
	bool               removed;
	uint64_t           reference_count;
};

/* Results from capability topology and authorization operations. */
enum cap_result {
	CAP_OK = 0,
	CAP_INVALID_ARGUMENTS,
	CAP_NOT_FOUND,
	CAP_NOT_AUTHORIZED,
	CAP_NOT_OWNER,
	CAP_REVOKED,
	CAP_RIGHTS_EXCEEDED,
	CAP_NO_MEMORY,
	CAP_ID_EXHAUSTED,
	CAP_OBJECT_DESTROYED,
};

/* Find or publish a userspace provider's object and return its stable ID. out_created reports whether this call
 *
 * installed a new record, allowing rollback without destroying a deduplicated object. */
cap_object_id_t cap_object_create(uint64_t object_id, struct channel* endpoint, bool* out_created);

/* Publish a kernel provider's object identifier through a handler and return its stable ID. */
cap_object_id_t cap_object_create_kernel(uint64_t object_id, cap_kernel_handler_t handler, bool* out_created);

/* Publish a managed kernel object and return its stable ID. */
cap_object_id_t cap_object_create_kernel_managed(uint64_t object_id, cap_kernel_handler_t handler,
                                                 cap_kernel_process_cleanup_t process_cleanup,
                                                 cap_kernel_destroy_t destroy, bool* out_created);

/* Publish a managed kernel object with a zero-grants lifecycle handler. */
cap_object_id_t cap_object_create_kernel_lifecycle(uint64_t object_id, cap_kernel_handler_t handler,
                                                   cap_kernel_process_cleanup_t process_cleanup,
                                                   cap_kernel_destroy_t         destroy,
                                                   cap_object_event_handler_t event_handler, bool* out_created);

/* Look up an existing object. The returned pointer is borrowed and requires external lifetime synchronisation. */
struct cap_object* cap_object_lookup(struct channel* endpoint, uint64_t object_id);

/* Retain a registered object so it cannot be finalized until cap_object_release(). */
struct cap_object* cap_object_acquire(cap_object_id_t id);

/* Release a reference returned by cap_object_acquire(). */
void cap_object_release(struct cap_object* object);

/* Unregister the routing object under id and remove capability subtrees that depend on it.
 * Reclaiming its metadata waits for retained references. */
bool cap_object_destroy_with_id(cap_object_id_t id);

/* Unregister a routing object. This never destroys the resource identified by object_id. */
bool cap_object_destroy(struct cap_object* object);

/* Unregister a published routing object only while it has no grants or active calls. */
bool cap_object_destroy_if_unused(struct cap_object* object);

/* Unpublish a userspace provider object identified by its endpoint and object ID. */
bool cap_object_unpublish(struct channel* endpoint, uint64_t object_id);

/* Unpublish a userspace provider object only while it has no grants or active calls. */
bool cap_object_unpublish_if_unused(struct channel* endpoint, uint64_t object_id);

/* Unpublish every routing object owned by endpoint. Represented provider resources remain untouched. */
void cap_object_unregister_endpoint(struct channel* endpoint);

/* Notify managed kernel objects before a process and its address space are destroyed. */
void cap_object_cleanup_for_process(process_id_t process);

/* Begin a capability call and snapshot its retained routing object and rights. */
enum cap_result cap_object_begin_call(process_id_t caller, struct capability* capability,
                                      struct cap_object** out_object, cap_rights_t* out_rights);

/* Resolve an authorized capability and retain its object for direct use. */
enum cap_result cap_object_acquire_for_use(process_id_t caller, cap_id_t capability_id, cap_rights_t required_rights,
                                           struct cap_object** out_object, cap_rights_t* out_rights);

/* End a previously begun capability call and release its routing-object reference. */
void cap_object_end_call(struct cap_object* object);

/* Prepare exclusive delivery of a queued zero-grants event, discarding stale input. */
bool cap_object_prepare_zero_grants_event(struct cap_object* object, uint64_t* out_object_id);

/* Commit delivery only if a prepared userspace zero-grants event remains current. */
bool cap_object_commit_zero_grants_event(struct cap_object* object);

/* Restore a prepared userspace zero-grants event after delivery failure. */
void cap_object_rollback_zero_grants_event(struct cap_object* object);

/* Discard one queued lifecycle event reference without notifying its provider. */
void cap_object_discard_event(struct cap_object* object);

/* Return the number of live grants referencing an object. */
size_t cap_object_grant_count(struct cap_object* object);

/* Return the number of active calls using an object. */
size_t cap_object_active_call_count(struct cap_object* object);

/* Create a fresh capability grant and return its stable ID. */
cap_id_t cap_create(cap_object_id_t cap_object_id, process_id_t target, cap_rights_t rights, struct capability* parent);

/* Delegate from source as either a child or a peer grant. Peer grants use source's parent as their parent. */
cap_id_t cap_delegate_create(struct capability* source, process_id_t target, cap_rights_t rights, bool peer);

/* Look up a capability by ID. The returned pointer is borrowed and requires external lifetime synchronisation. */
struct capability* cap_lookup(cap_id_t id);

/* Acquire a capability record by ID so concurrent removal cannot reclaim it. */
struct capability* cap_acquire(cap_id_t id);

/* Release a record returned by cap_acquire(). */
void cap_release(struct capability* capability);

/* Synchronised accessors for mutable grant state. Immutable fields require a retained record. */
cap_rights_t cap_rights(const struct capability* capability);

/* Check if a capability has been removed. */
bool cap_is_removed(const struct capability* capability);

/* Remove rights from capability and every descendant. */
bool cap_remove_rights(struct capability* capability, cap_rights_t rights);

/* Remove a capability and its delegation subtree. Retained records remain alive until their final cap_release(). */
bool cap_destroy(struct capability* capability);

/* Destroy a capability whose ID is already known, without dereferencing the supplied pointer. */
bool cap_destroy_by_id(cap_id_t id);

/* Drop one capability handle while preserving its descendants by splicing them into its parent. */
bool cap_drop(struct capability* capability);

/* Drop every capability held by target while preserving grants delegated to other processes. */
void cap_drop_for_process(process_id_t target);

/* Check whether caller is authorized to use a capability by walking the parent chain. */
enum cap_result cap_is_authorized(process_id_t caller, struct capability* cap);

/* Validate that a retained capability record is still part of the live capability topology. */
enum cap_result cap_is_valid(struct capability* cap);

/* Return true when the cap_object backing cap is still registered. */
bool cap_object_alive(struct capability* cap);

/* Initialize the global capability and cap_object ID tables. */
void capability_init(void);

/* Number of registered kernel objects. */
size_t capability_object_count(void);

/* Number of live capabilities. */
size_t capability_count(void);
