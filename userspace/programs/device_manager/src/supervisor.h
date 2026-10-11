#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device.h"
#include "driver.h"

struct cap_request;
struct device_server;

/* Observable lifecycle state retained for one device-to-driver binding. */
enum dm_driver_binding_state {
	DM_DRIVER_UNBOUND = 0u,
	DM_DRIVER_STARTING,
	DM_DRIVER_RUNNING,
	DM_DRIVER_BACKOFF,
	DM_DRIVER_WAITING_FOR_CHILDREN,
	DM_DRIVER_STOPPING,
	DM_DRIVER_STOPPED,
	DM_DRIVER_QUARANTINED,
};

/* Bounded tail of bytes written by one driver to one standard stream. */
struct dm_driver_log {
	uint8_t* data;
	size_t   size;
};

/* Runtime state for the unique driver selected for one immutable device. */
struct dm_driver_binding {
	struct dm_device*            device;
	struct dm_driver*            driver;
	enum dm_driver_binding_state state;
	uint64_t                     generation;
	unsigned int                 restart_count;
	process_id_t                 process_id;
	cap_id_t                     process_cap;
	syscall_status_t             last_wait_status;
	uintptr_t                    last_exit_code;
	struct dm_driver_log         stdout_log;
	struct dm_driver_log         stderr_log;
	struct dm_driver_binding*    next;
};

/* Private objects and all live binding state owned by the device manager. */
struct dm_driver_supervisor {
	cap_id_t                  self_cap;
	cap_id_t                  control_cap;
	cap_id_t                  stdout_cap;
	cap_id_t                  stderr_cap;
	struct dm_driver_binding* bindings;
};

/* Publish private control and driver-output objects on the server endpoint. */
bool dm_supervisor_init(struct device_server* server, cap_id_t self_cap);

/* Release private objects, retained logs, and process-control authorities. */
void dm_supervisor_deinit(struct device_server* server);

/* Match and start drivers for all currently unbound devices. */
void dm_supervisor_reconcile(struct device_server* server);

/* Match a newly committed device after its construction reply succeeds. */
void dm_supervisor_device_committed(struct device_server* server, struct dm_device* device);

/* Validate that a child transaction belongs to the parent's active driver generation. */
bool dm_supervisor_authorize_child(struct device_server* server, const struct dm_device* parent, process_id_t caller,
                                   struct dm_driver_binding** out_binding, uint64_t* out_generation);

/* Dispatch a private supervisor-control or driver-output call. */
bool dm_supervisor_dispatch(struct device_server* server, const struct cap_request* call, const void* request);
