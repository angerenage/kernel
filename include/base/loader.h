#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

/* Request to load an ELF image represented by a boot-module capability. */
struct loader_load_request {
	cap_id_t module_cap;
};

/* Process control and runtime bootstrap data produced by the loader. */
struct loader_load_response {
	cap_id_t  process_cap;
	cap_id_t  address_space_cap;
	uintptr_t entry;
	uintptr_t heap_base;
	size_t    heap_page_count;
};
