#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/input_manager.h>
#include <stddef.h>
#include <stdint.h>

/* Metadata accompanying one validated aggregate-event page. */
struct input_manager_event_page {
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	size_t   returned;
	uint32_t flags;
};

/* Read aggregate metadata and the current coherent topology cursor. */
syscall_status_t input_manager_get_info(cap_id_t manager_cap, struct input_manager_get_info_response* out_info);

/* Enumerate one page of sources from an exact topology generation. */
syscall_status_t input_manager_enumerate_sources(cap_id_t manager_cap, uint64_t topology_generation, uint64_t offset,
                                                 struct input_manager_source* sources, size_t capacity,
                                                 size_t* out_returned, uint64_t* out_total);

/* Read one page of immutable descriptors for a source in an exact generation. */
syscall_status_t input_manager_read_descriptors(cap_id_t manager_cap, uint64_t topology_generation, uint64_t source_id,
                                                uint64_t offset, struct input_control_descriptor* descriptors,
                                                size_t capacity, size_t* out_returned, uint64_t* out_total);

/* Read retained aggregate events newer than after_sequence. */
syscall_status_t input_manager_read_events(cap_id_t manager_cap, uint64_t after_sequence,
                                           struct input_manager_event* events, size_t capacity,
                                           struct input_manager_event_page* out_page);

/* Acquire the aggregate service's coalesced event signal with CAP_WAIT. */
syscall_status_t input_manager_acquire_event_signal(cap_id_t manager_cap, cap_id_t* out_signal_cap);
