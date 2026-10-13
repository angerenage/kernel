#pragma once

#include <base/channel.h>
#include <base/process.h>
#include <protocol/input.h>
#include <protocol/input_manager.h>
#include <runtime/init.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed limits for retained aggregate state and attached provider metadata. */
#define INPUT_MANAGER_EVENT_QUEUE_CAPACITY 4096u
#define INPUT_MANAGER_MAXIMUM_FRAME_EVENTS 128u
#define INPUT_MANAGER_MAXIMUM_DESCRIPTORS 1024u
#define INPUT_MANAGER_MAXIMUM_SOURCES 1024u
#define INPUT_MANAGER_OBJECT_ID 1u

/* One registry identity and its validated, process-lifetime attachment state. */
struct managed_input_source {
	struct managed_input_source*     next;
	struct init_service_info         service;
	process_id_t                     owner;
	uint64_t                         id;
	enum input_device_class          device_class;
	struct input_control_descriptor* descriptors;
	size_t                           descriptor_count;
	cap_id_t                         input_cap;
	cap_id_t                         signal_cap;
	cap_id_t                         waiter_cap;
	uint64_t                         cursor;
	uint64_t                         pending_frame;
	uint64_t                         last_frame;
	struct input_manager_event       pending[INPUT_MANAGER_MAXIMUM_FRAME_EVENTS];
	size_t                           pending_count;
	bool                             active;
	bool                             quarantined;
	bool                             seen;
	bool                             dirty;
	bool                             dead;
	bool                             stopping;
	uint64_t                         identity_revision;
};

/* State shared by the serialized aggregation loop and capability server. */
struct input_manager {
	process_id_t                 pid;
	cap_id_t                     self_cap;
	channel_id_t                 endpoint;
	cap_id_t                     channel_activity_cap;
	cap_id_t                     service_cap;
	cap_id_t                     event_signal_cap;
	cap_id_t                     activity_cap;
	cap_id_t                     registry_signal_cap;
	uint64_t                     registry_subscription;
	bool                         registry_dirty;
	bool                         registry_failed;
	bool                         locked;
	uint64_t                     registry_revision;
	uint64_t                     topology_generation;
	uint64_t                     next_source_id;
	uint64_t                     next_sequence;
	uint64_t                     next_frame;
	struct input_manager_event*  events;
	size_t                       event_tail;
	size_t                       event_count;
	struct managed_input_source* sources;
	size_t                       source_count;
};

/* Initialize, advertise, and begin watching raw input services. */
bool input_manager_initialize(struct input_manager* manager);

/* Reconcile and aggregate sources until an unrecoverable manager failure. */
int input_manager_run(struct input_manager* manager);

/* Serialize access shared by the aggregation loop and capability server. */
void input_manager_lock(struct input_manager* manager);

/* Release serialized manager access. */
void input_manager_unlock(struct input_manager* manager);

/* Start the aggregate capability server on a detached thread. */
bool input_manager_server_start(struct input_manager* manager);
