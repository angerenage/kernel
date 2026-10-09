#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stdbool.h>

#include "device.h"

/* Channel ownership, private root authority, and device state for one server instance. */
struct device_server {
	channel_id_t    endpoint;
	cap_id_t        activity;
	cap_id_t        root_cap;
	bool            root_published;
	process_id_t    pid;
	struct dm_state state;
};

/* Create the endpoint and publish the root constructor only to the manager itself. */
bool device_server_init(struct device_server* server);

/* Unpublish every object and release all channel and device state. */
void device_server_deinit(struct device_server* server);

/* Serve device and builder calls until a channel or activity error occurs. */
int device_server_run(struct device_server* server);

/* Internal hook for the future binding layer; device IDs remain private to the manager. */
syscall_status_t device_server_grant(struct device_server* server, uint64_t device_id, process_id_t target,
                                     cap_rights_t rights, cap_id_t* out_cap);

/* Dispatch one already-received request; exposed for native protocol tests. */
bool device_server_dispatch(struct device_server* server, const struct cap_request* call, const void* request);

/* Abort an unfinished builder after its final external grant disappears. */
void device_server_handle_event(struct device_server* server, const struct channel_event* event);
