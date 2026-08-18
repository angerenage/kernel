#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct loader_loaded_program {
	cap_id_t                      load_cap;
	cap_id_t                      process_cap;
	cap_id_t                      address_space_cap;
	process_id_t                  process_id;
	uintptr_t                     entry;
	uintptr_t                     heap_base;
	size_t                        heap_page_count;
	cap_id_t                      init_cap;
	cap_id_t                      serial_cap;
	cap_id_t*                     allocation_caps;
	size_t                        allocation_count;
	bool                          started;
	struct loader_loaded_program* next;
};

/* Build a complete, non-running process from a static ELF64 Blob. */
syscall_status_t loader_prepare_program(cap_id_t blob_cap, const char* name, size_t name_size,
                                        struct loader_loaded_program** out_program);

/* Start a prepared process with the serialized argv payload from LOADER_V1_OP_RUN. */
syscall_status_t loader_start_program(struct loader_loaded_program* program, uint32_t argc, const void* argv_data,
                                      size_t argv_size, cap_id_t* out_thread_cap);

/* Destroy a prepared process and release allocations retained by the loader. */
void loader_discard_program(struct loader_loaded_program* program);
