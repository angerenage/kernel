#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <core/channel.h>
#include <core/id_table.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A kernel or userspace object that can be referenced by capabilities. */
struct cap_object {
	id_table_id_t   cap_object_id;
	uint64_t        object_id;
	struct channel* endpoint;
};

/* A capability grants a target process rights on a cap_object. Capabilities form a delegation tree through parent. */
struct capability {
	cap_id_t           cap_id;
	struct cap_object* object;
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
};

/* Create a new kernel object and register it in the global object table. Returns NULL on failure. */
struct cap_object* cap_object_create(uint64_t object_id, struct channel* endpoint);

/* Look up an existing kernel object by its endpoint and object_id. */
struct cap_object* cap_object_lookup(struct channel* endpoint, uint64_t object_id);

/* Destroy a kernel object and remove it from the global object table. */
bool cap_object_destroy(struct cap_object* object);

/* Create a new capability granting rights on an object to a target process. */
struct capability* cap_create(struct cap_object* object, process_id_t target, cap_rights_t rights,
                              struct capability* parent);

/* Look up a capability by its global ID. */
struct capability* cap_lookup(cap_id_t id);

/* Destroy a capability and remove it from the global capability table. */
bool cap_destroy(struct capability* capability);

/* Check whether caller is authorized to use a capability by walking the parent chain. */
enum cap_result cap_is_authorized(process_id_t caller, struct capability* cap);

/* Validate a capability by checking that neither it nor any ancestor is revoked. */
enum cap_result cap_is_valid(struct capability* cap);

/* Initialize the global capability and cap_object ID tables. */
void capability_init(void);

/* Number of registered kernel objects. */
size_t capability_object_count(void);

/* Number of live capabilities. */
size_t capability_count(void);
