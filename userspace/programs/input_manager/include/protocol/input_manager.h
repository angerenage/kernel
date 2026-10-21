#pragma once

/*
 * Layout-independent aggregate input protocol.
 *
 * The input manager combines dynamically changing input-device services into
 * one retained stream while preserving device identity, physical controls,
 * and provider frame boundaries. It also routes synchronous device-output
 * requests without assigning text, layout, modifier, lock, or repeat policy.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <protocol/input.h>
#include <stdint.h>

/* Registry identity of the single system-wide aggregate input service. */
#define INPUT_MANAGER_NAMESPACE "input"
#define INPUT_MANAGER_PROTOCOL_NAME "manager"
#define INPUT_MANAGER_SERVICE_NAME "default"

/* Exact rights granted to aggregate-input clients and their event signal. */
#define INPUT_MANAGER_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE))
#define INPUT_MANAGER_EVENT_SIGNAL_RIGHTS ((cap_rights_t)CAP_WAIT)

/* One currently attached input source in a coherent topology snapshot. */
struct input_manager_source {
	uint64_t                id;
	enum input_device_class device_class;
	uint32_t                descriptor_count;
	uint32_t                output_descriptor_count;
	uint32_t                maximum_output_values;
};

/* Kinds of records retained in the aggregate event history. */
enum input_manager_event_type {
	INPUT_MANAGER_EVENT_INVALID = 0,
	INPUT_MANAGER_EVENT_SOURCE_ADDED,
	INPUT_MANAGER_EVENT_SOURCE_REMOVED,
	INPUT_MANAGER_EVENT_SOURCE_RESET,
	INPUT_MANAGER_EVENT_CONTROL,
};

/* Flags attached to one aggregate event record. */
enum input_manager_event_flags {
	INPUT_MANAGER_EVENT_FLAG_NONE      = 0u,
	INPUT_MANAGER_EVENT_FLAG_FRAME_END = 1u << 0u,
};

/* One source lifecycle change or normalized control value. */
struct input_manager_event {
	uint64_t                      sequence;
	uint64_t                      frame;
	uint64_t                      timestamp_ns;
	uint64_t                      source_id;
	enum input_manager_event_type type;
	enum input_device_class       device_class;
	uint32_t                      control_id;
	enum input_control_kind       control_kind;
	uint32_t                      control_code;
	uint32_t                      group_id;
	uint32_t                      flags;
	int64_t                       value;
	int64_t                       logical_minimum;
	int64_t                       logical_maximum;
	uint32_t                      reserved;
};

/* Operations accepted by the aggregate input service capability. */
enum input_manager_op {
	INPUT_MANAGER_OP_GET_INFO = 0,
	INPUT_MANAGER_OP_ENUMERATE_SOURCES,
	INPUT_MANAGER_OP_READ_DESCRIPTORS,
	INPUT_MANAGER_OP_READ_EVENTS,
	INPUT_MANAGER_OP_ACQUIRE_EVENT_SIGNAL,
	INPUT_MANAGER_OP_READ_OUTPUT_DESCRIPTORS,
	INPUT_MANAGER_OP_SET_OUTPUTS,
};

/* Common operation header for aggregate input requests. */
struct input_manager_request_header {
	enum input_manager_op op;
};

/* Request aggregate metadata and a coherent topology cursor. */
struct input_manager_get_info_request {
	struct input_manager_request_header header;
};

/* Current aggregate metadata returned by INPUT_MANAGER_OP_GET_INFO. */
struct input_manager_get_info_response {
	uint64_t topology_generation;
	uint64_t source_count;
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	uint32_t queue_capacity;
	uint32_t maximum_frame_events;
};

/* Request one page from an exact topology generation. */
struct input_manager_enumerate_sources_request {
	struct input_manager_request_header header;
	uint32_t                            reserved;
	uint64_t                            topology_generation;
	uint64_t                            offset;
	uint64_t                            count;
};

/* Source entries follow this successful response header. */
struct input_manager_enumerate_sources_response {
	uint64_t                    topology_generation;
	uint64_t                    total;
	uint64_t                    returned;
	struct input_manager_source sources[];
};

/* Request immutable descriptors for one source in an exact topology generation. */
struct input_manager_read_descriptors_request {
	struct input_manager_request_header header;
	uint32_t                            reserved;
	uint64_t                            topology_generation;
	uint64_t                            source_id;
	uint64_t                            offset;
	uint64_t                            count;
};

/* Control descriptors follow this successful response header. */
struct input_manager_read_descriptors_response {
	uint64_t                        topology_generation;
	uint64_t                        total;
	uint64_t                        returned;
	struct input_control_descriptor descriptors[];
};

/* Request retained aggregate records newer than after_sequence. */
struct input_manager_read_events_request {
	struct input_manager_request_header header;
	uint32_t                            reserved;
	uint64_t                            after_sequence;
	uint64_t                            count;
};

/* Conditions reported with a successful aggregate event read. */
enum input_manager_read_event_flags {
	INPUT_MANAGER_READ_EVENT_FLAG_NONE = 0u,
	INPUT_MANAGER_READ_EVENT_FLAG_GAP  = 1u << 0u,
};

/* Aggregate event records follow this successful response header. */
struct input_manager_read_events_response {
	uint64_t                   oldest_sequence;
	uint64_t                   newest_sequence;
	uint64_t                   returned;
	uint32_t                   flags;
	uint32_t                   reserved;
	struct input_manager_event events[];
};

/* Request the coalesced signal announcing newly retained records. */
struct input_manager_acquire_event_signal_request {
	struct input_manager_request_header header;
};

/* Event signal delegated to the caller with exactly CAP_WAIT. */
struct input_manager_acquire_event_signal_response {
	cap_id_t capability;
};

/* Request output descriptors for one source in an exact topology generation. */
struct input_manager_read_output_descriptors_request {
	struct input_manager_request_header header;
	uint32_t                            reserved;
	uint64_t                            topology_generation;
	uint64_t                            source_id;
	uint64_t                            offset;
	uint64_t                            count;
};

/* Output descriptors follow this successful response header. */
struct input_manager_read_output_descriptors_response {
	uint64_t                       topology_generation;
	uint64_t                       total;
	uint64_t                       returned;
	struct input_output_descriptor descriptors[];
};

/* Route one atomic output batch to a source in an exact topology generation. */
struct input_manager_set_outputs_request {
	struct input_manager_request_header header;
	uint32_t                            reserved;
	uint64_t                            topology_generation;
	uint64_t                            source_id;
	uint64_t                            count;
	struct input_output_value           values[];
};
