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

/* A kernel or userspace object that can be referenced by capabilities. */
struct cap_object {
	id_table_id_t        cap_object_id;
	uint64_t             object_id;
	struct channel*      endpoint;
	cap_kernel_handler_t handler;
};

/* A capability grants a target process rights on a cap_object. Capabilities form a delegation tree through parent. */
struct capability {
	cap_id_t           cap_id;
	id_table_id_t      cap_object_id;
	process_id_t       target;
	cap_rights_t       rights;
	struct capability* parent;
	bool               revoked;
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

/* Create a new userspace-owned object and register it in the global object table. Returns NULL on failure. */
struct cap_object* cap_object_create(uint64_t object_id, struct channel* endpoint);

/* Create a new kernel-owned object with a handler and an endpoint set to NULL. Returns NULL on failure. */
struct cap_object* cap_object_create_kernel(uint64_t object_id, cap_kernel_handler_t handler);

/* Look up an existing kernel object by its endpoint and object_id. */
struct cap_object* cap_object_lookup(struct channel* endpoint, uint64_t object_id);

/* Return the cap_object registered under id, or NULL if no such object is registered. */
struct cap_object* cap_object_get(id_table_id_t id);

/* Remove the cap_object registered under id from the global table and free it. Returns true when an object was removed.
 */
bool cap_object_destroy_with_id(id_table_id_t id);

/* Destroy a kernel object and remove it from the global object table. */
bool cap_object_destroy(struct cap_object* object);

/* Create a new capability granting rights on cap_object_id to target. The capability does not take ownership of the
 * object. */
struct capability* cap_create(id_table_id_t cap_object_id, process_id_t target, cap_rights_t rights,
                              struct capability* parent);

/* Look up a capability by its global ID. */
struct capability* cap_lookup(cap_id_t id);

/* Destroy a capability and remove it from the global capability table. */
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
