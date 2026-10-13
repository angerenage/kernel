#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/input.h>
#include <stddef.h>
#include <stdint.h>

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
