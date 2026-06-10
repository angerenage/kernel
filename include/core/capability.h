#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <core/channel.h>
#include <core/id_table.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cap_object {
	id_table_id_t   cap_object_id;
	uint64_t        object_id;
	struct channel* endpoint;
};

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

struct cap_object* cap_object_create(uint64_t object_id, struct channel* endpoint);
struct cap_object* cap_object_lookup(struct channel* endpoint, uint64_t object_id);
bool               cap_object_destroy(struct cap_object* object);

struct capability* cap_create(struct cap_object* object, process_id_t target, cap_rights_t rights,
                              struct capability* parent);
struct capability* cap_lookup(cap_id_t id);
bool               cap_destroy(struct capability* capability);

enum cap_result cap_is_authorized(process_id_t caller, struct capability* cap);
enum cap_result cap_is_valid(struct capability* cap);

void   capability_init(void);
size_t capability_object_count(void);
size_t capability_count(void);
