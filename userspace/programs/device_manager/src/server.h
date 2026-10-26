#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stdbool.h>

#include "device.h"
#include "driver.h"
#include "supervisor.h"

/* A superseded catalog retained while live bindings still reference its declarations. */
struct dm_retired_driver_catalog {
	struct dm_driver_catalog          catalog;
	struct dm_retired_driver_catalog* next;
};

/* Channel ownership, private root authority, device state, and catalog for one server instance. */
struct device_server {
	channel_id_t                      endpoint;
	cap_id_t                          activity;
	cap_id_t                          root_cap;
	cap_id_t                          service_cap;
	cap_id_t                          memory_allocator_cap;
	cap_id_t                          dma_cap;
	cap_id_t                          interrupts_cap;
	cap_id_t                          io_ports_cap;
	bool                              root_published;
	bool                              service_advertised;
	process_id_t                      pid;
	uint64_t                          catalog_generation;
	struct dm_state                   state;
	struct dm_driver_catalog          drivers;
	struct dm_retired_driver_catalog* retired_drivers;
	struct dm_driver_supervisor       supervisor;
};

/* Create the endpoint and publish the root constructor only to the manager itself. */
bool device_server_init(struct device_server* server);

/* Advertise the public read-only and init-grantable control service. */
bool device_server_advertise(struct device_server* server);

/* Unpublish every object and release all channel, device, and catalog state. */
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
