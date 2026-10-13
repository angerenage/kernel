#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/input.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum number of updates accepted by the runtime provider in one frame. */
#define INPUT_PROVIDER_MAX_FRAME_EVENTS 128u

/* Opaque process-lifetime input provider created by a hardware driver. */
struct input_provider;

/* One control value supplied while constructing an input frame. */
struct input_control_value {
	uint32_t control_id;
	int64_t  value;
};

/* Metadata accompanying one validated retained-event page. */
struct input_event_page {
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	size_t   returned;
	uint32_t flags;
};

/* Read immutable input-service metadata. */
syscall_status_t input_get_info(cap_id_t input_cap, struct input_get_info_response* out_info);

/* Read one provider-ordered page of immutable control descriptors. */
syscall_status_t input_read_descriptors(cap_id_t input_cap, uint64_t offset,
                                        struct input_control_descriptor* descriptors, size_t capacity,
                                        size_t* out_returned, uint64_t* out_total);

/* Read retained events newer than after_sequence without advancing provider state. */
syscall_status_t input_read_events(cap_id_t input_cap, uint64_t after_sequence, struct input_event* events,
                                   size_t capacity, struct input_event_page* out_page);

/* Acquire the producer's coalesced event-notification signal with CAP_WAIT. */
syscall_status_t input_acquire_event_signal(cap_id_t input_cap, cap_id_t* out_signal_cap);

/* Create and advertise a provider after copying its immutable descriptors. */
syscall_status_t input_provider_create(const char* service, uint32_t protocol_major, uint32_t protocol_minor,
                                       enum input_device_class                device_class,
                                       const struct input_control_descriptor* descriptors, size_t descriptor_count,
                                       struct input_provider** out_provider);

/* Atomically append one nonempty coherent frame and notify waiting clients. */
syscall_status_t input_provider_publish(struct input_provider* provider, const struct input_control_value* values,
                                        size_t count);

/* Append a state-reset frame after loss, reinitialization, or desynchronization. */
syscall_status_t input_provider_reset(struct input_provider* provider);
