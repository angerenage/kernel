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
typedef bool (*cap_kernel_process_cleanup_t)(uint64_t object_id, process_id_t process);
typedef void (*cap_kernel_destroy_t)(uint64_t object_id);

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
	uint64_t                     reference_count;
};

/* A capability grants a target process rights on a cap_object. Capabilities form a delegation tree through parent. */
struct capability {
	cap_id_t           cap_id;
	cap_object_id_t    cap_object_id;
	process_id_t       target;
	cap_rights_t       rights;
	struct capability* parent;
	bool               revoked;
	uint64_t           reference_count;
};

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

/*
 * Find or publish a userspace provider's object through an endpoint. Returns NULL on failure.
 * When out_created is
 * non-NULL, it reports whether this call installed the returned table record. This allows
 * callers to roll back their
 * own creation without destroying an older record returned by deduplication.
 */
struct cap_object* cap_object_create(uint64_t object_id, struct channel* endpoint, bool* out_created);

/* Publish a kernel provider's object identifier through a handler. Returns NULL on failure. */
struct cap_object* cap_object_create_kernel(uint64_t object_id, cap_kernel_handler_t handler, bool* out_created);

/* Publish a kernel object with resource lifecycle callbacks. */
struct cap_object* cap_object_create_kernel_managed(uint64_t object_id, cap_kernel_handler_t handler,
                                                    cap_kernel_process_cleanup_t process_cleanup,
                                                    cap_kernel_destroy_t destroy, bool* out_created);

/* Look up an existing object. The returned pointer is borrowed and requires external lifetime synchronisation. */
struct cap_object* cap_object_lookup(struct channel* endpoint, uint64_t object_id);

/* Retain a registered object so it cannot be finalized until cap_object_release(). */
struct cap_object* cap_object_acquire(cap_object_id_t id);

/* Release a reference returned by cap_object_acquire(). */
void cap_object_release(struct cap_object* object);

/* Unregister the routing object under id. Reclaiming its metadata waits for retained references. */
bool cap_object_destroy_with_id(cap_object_id_t id);

/* Unregister a routing object. This never destroys the resource identified by object_id. */
bool cap_object_destroy(struct cap_object* object);

/* Unpublish every routing object owned by endpoint. Represented provider resources remain untouched. */
void cap_object_unregister_endpoint(struct channel* endpoint);

/* Notify managed kernel objects before a process and its address space are destroyed. */
void cap_object_cleanup_for_process(process_id_t process);

/*
 * Create or deduplicate a capability grant. The returned pointer is table-owned and the capability does not retain
 *
 * the represented cap_object. When out_created is non-NULL, it reports whether this call installed a fresh record,
 *
 * allowing precise rollback without destroying an older identical grant.
 */
struct capability* cap_create(cap_object_id_t cap_object_id, process_id_t target, cap_rights_t rights,
                              struct capability* parent, bool* out_created);

/* Look up a capability by ID. The returned pointer is borrowed and requires external lifetime synchronisation. */
struct capability* cap_lookup(cap_id_t id);

/* Acquire a capability record by ID so concurrent removal cannot reclaim it. */
struct capability* cap_acquire(cap_id_t id);

/* Release a record returned by cap_acquire(). */
void cap_release(struct capability* capability);

/* Synchronised accessors for mutable grant state. Immutable fields require a retained record. */
cap_rights_t cap_rights(const struct capability* capability);
bool         cap_is_revoked(const struct capability* capability);
void         cap_mark_revoked(struct capability* capability);
bool         cap_remove_rights(struct capability* capability, cap_rights_t rights);

/* Remove a capability from the table. The pointer must already be protected by ownership or a retained reference. */
bool cap_destroy(struct capability* capability);

/* Destroy a capability whose ID is already known, without dereferencing the supplied pointer. */
bool cap_destroy_by_id(cap_id_t id);

/* Mark every capability whose target is the given process as revoked. Capability records are not freed here. */
void cap_revoke_for_process(process_id_t target);

/* Check whether caller is authorized to use a capability by walking the parent chain. */
enum cap_result cap_is_authorized(process_id_t caller, struct capability* cap);

/* Validate a capability by checking that the underlying cap_object is still registered and that no ancestor is revoked.
 */
enum cap_result cap_is_valid(struct capability* cap);

/* Return true when the cap_object backing cap is still registered. */
bool cap_object_alive(struct capability* cap);

/* Initialize the global capability and cap_object ID tables. */
void capability_init(void);

/* Number of registered kernel objects. */
size_t capability_object_count(void);

/* Number of live capabilities. */
size_t capability_count(void);
