#include "load.h"

#include <base/heap.h>
#include <base/math.h>
#include <base/startup.h>
#include <base/thread.h>
#include <runtime/blob.h>
#include <runtime/heap.h>
#include <runtime/init.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/display.h>
#include <system/memory.h>
#include <system/process.h>

#include "elf64.h"
#include "load_plan.h"

static syscall_status_t parse_status(enum elf64_parse_result result) {
	switch (result) {
	case ELF64_PARSE_OK:
		return SYSCALL_STATUS_OK;
	case ELF64_PARSE_INVALID_ARGUMENT:
	case ELF64_PARSE_BAD_FORMAT:
		return SYSCALL_STATUS_BAD_ARGUMENT;
	case ELF64_PARSE_UNSUPPORTED:
		return SYSCALL_STATUS_UNAVAILABLE;
	default:
		return SYSCALL_STATUS_FAILED;
	}
}

static memory_access_t segment_access(uint32_t flags) {
	return ((flags & ELF64_SEGMENT_READ) != 0u ? MEMORY_ACCESS_READ : 0u) |
	       ((flags & ELF64_SEGMENT_WRITE) != 0u ? MEMORY_ACCESS_WRITE : 0u) |
	       ((flags & ELF64_SEGMENT_EXEC) != 0u ? MEMORY_ACCESS_EXEC : 0u);
}

static cap_rights_t mapping_rights(memory_access_t access) {
	return CAP_CALL | CAP_MAP | CAP_DESTROY | CAP_DELEGATE | ((access & MEMORY_ACCESS_READ) != 0u ? CAP_READ : 0u) |
	       ((access & MEMORY_ACCESS_WRITE) != 0u ? CAP_WRITE : 0u) |
	       ((access & MEMORY_ACCESS_EXEC) != 0u ? CAP_EXEC : 0u);
}

static syscall_status_t plan_status(enum loader_elf_plan_result result) {
	switch (result) {
	case LOADER_ELF_PLAN_OK:
		return SYSCALL_STATUS_OK;
	case LOADER_ELF_PLAN_INVALID_ARGUMENT:
		return SYSCALL_STATUS_BAD_ARGUMENT;
	case LOADER_ELF_PLAN_BAD_LAYOUT:
		return SYSCALL_STATUS_UNAVAILABLE;
	default:
		return SYSCALL_STATUS_FAILED;
	}
}

static bool segment_in_region(const struct elf64_segment* segment, const struct loader_elf_load_region* region) {
	uint64_t region_end, segment_end;
	return segment->memsz != 0u && !add_overflow_u64(region->virtual_base, region->size, &region_end) &&
	       !add_overflow_u64(segment->vaddr, segment->memsz, &segment_end) && segment->vaddr >= region->virtual_base &&
	       segment_end <= region_end;
}

static syscall_status_t record_mapping(struct loader_loaded_program* program, cap_id_t cap) {
	struct loader_mapping_authority* authority = malloc(sizeof(*authority));
	if (authority == NULL) return SYSCALL_STATUS_FAILED;
	authority->cap    = cap;
	authority->next   = program->mappings;
	program->mappings = authority;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t populate_region(cap_id_t blob_cap, uintptr_t destination, const struct elf64_image* image,
                                        const struct loader_elf_load_region* region) {
	for (size_t i = 0u; i < image->segment_count; i++) {
		const struct elf64_segment* segment = &image->segments[i];
		if (!segment_in_region(segment, region) || segment->filesz == 0u) continue;
		uint64_t offset = segment->vaddr - region->virtual_base;
		if (offset > SIZE_MAX || segment->filesz > SIZE_MAX ||
		    blob_read(blob_cap, segment->offset, (void*)(destination + (size_t)offset), (size_t)segment->filesz) !=
		        SYSCALL_STATUS_OK)
			return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

static syscall_status_t load_region(struct loader_loaded_program* program, cap_id_t blob_cap,
                                    const struct elf64_image* image, const struct loader_elf_load_plan* plan,
                                    const struct loader_elf_load_region* region) {
	cap_id_t                          memory_cap = CAP_ID_INVALID;
	struct address_space_map_response temporary  = {.mapping_cap = CAP_ID_INVALID};
	syscall_status_t                  status;
	if (region->virtual_base > UINTPTR_MAX) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = memory_allocator_alloc(runtime_heap_memory_allocator_cap, region->size, &memory_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	status = address_space_map(runtime_heap_address_space_cap,
	                           memory_cap,
	                           MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                           0u,
	                           program->mapping_granule,
	                           0u,
	                           0u,
	                           &temporary);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	status = populate_region(blob_cap, temporary.address, image, region);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	status = mapping_unmap(temporary.mapping_cap);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	temporary.mapping_cap = CAP_ID_INVALID;

	for (size_t i = 0u; i < region->run_count; i++) {
		const struct loader_elf_load_run* run    = &plan->runs[region->first_run + i];
		cap_id_t                          view   = memory_cap;
		struct address_space_map_response mapped = {.mapping_cap = CAP_ID_INVALID};
		if (run->object_offset != 0u || run->size != region->size) {
			status = memory_slice(memory_cap, run->object_offset, run->size, &view);
			if (status != SYSCALL_STATUS_OK) goto cleanup;
		}
		status = address_space_map(program->address_space_cap,
		                           view,
		                           run->access,
		                           (uintptr_t)(region->virtual_base + run->object_offset),
		                           program->mapping_granule,
		                           0u,
		                           0u,
		                           &mapped);
		if (view != memory_cap) (void)cap_drop(view);
		if (status != SYSCALL_STATUS_OK) goto cleanup;
		status = record_mapping(program, mapped.mapping_cap);
		if (status != SYSCALL_STATUS_OK) {
			(void)mapping_unmap(mapped.mapping_cap);
			goto cleanup;
		}
	}
	status = cap_drop(memory_cap);
	if (status == SYSCALL_STATUS_OK) memory_cap = CAP_ID_INVALID;
	return status;
cleanup:
	if (temporary.mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(temporary.mapping_cap);
	if (memory_cap != CAP_ID_INVALID) (void)cap_drop(memory_cap);
	return status;
}

static syscall_status_t allocate_heap(struct loader_loaded_program* program) {
	cap_id_t                          memory_cap = CAP_ID_INVALID;
	struct address_space_map_response mapped     = {.mapping_cap = CAP_ID_INVALID};
	size_t                            size;
	if (!align_up_size(HEAP_DEFAULT_GROW_SIZE, program->mapping_granule, &size)) return SYSCALL_STATUS_FAILED;
	syscall_status_t status = memory_allocator_alloc(runtime_heap_memory_allocator_cap, size, &memory_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	status = address_space_map(
		program->address_space_cap, memory_cap, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, 0u, 0u, 0u, 0u, &mapped);
	if (status != SYSCALL_STATUS_OK || mapped.address == 0u) goto cleanup;
	status = record_mapping(program, mapped.mapping_cap);
	if (status != SYSCALL_STATUS_OK) {
		(void)mapping_unmap(mapped.mapping_cap);
		goto cleanup;
	}
	status = cap_drop(memory_cap);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	program->heap_base = mapped.address;
	program->heap_size = size;
	return SYSCALL_STATUS_OK;
cleanup:
	if (memory_cap != CAP_ID_INVALID) (void)cap_drop(memory_cap);
	return status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status;
}

static syscall_status_t delegate_runtime_caps(struct loader_loaded_program* program) {
	if (init_cap_id == CAP_ID_INVALID || serial_cap_id == CAP_ID_INVALID ||
	    runtime_heap_memory_allocator_cap == CAP_ID_INVALID)
		return SYSCALL_STATUS_UNAVAILABLE;
	syscall_status_t status = cap_delegate_peer(init_cap_id, program->process_id, CAP_CALL, &program->init_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	status = cap_delegate_peer(serial_cap_id, program->process_id, CAP_CALL | CAP_WRITE, &program->serial_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	return cap_delegate(runtime_heap_memory_allocator_cap,
	                    program->process_id,
	                    CAP_CALL | CAP_READ | CAP_ALLOCATE,
	                    &program->memory_allocator_cap);
}

static syscall_status_t transfer_mappings(struct loader_loaded_program* program) {
	for (struct loader_mapping_authority* item = program->mappings; item != NULL; item = item->next) {
		struct mapping_info info;
		cap_id_t            delegated;
		syscall_status_t    status = mapping_info(item->cap, &info);
		if (status != SYSCALL_STATUS_OK) return status;
		status = cap_delegate_peer(item->cap, program->process_id, mapping_rights(info.access), &delegated);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	while (program->mappings != NULL) {
		struct loader_mapping_authority* item = program->mappings;
		program->mappings                     = item->next;
		syscall_status_t status               = cap_drop(item->cap);
		free(item);
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}

static bool argv_payload_valid(uint32_t argc, const void* data, size_t size) {
	const uint8_t* cursor = data;
	const uint8_t* end;
	if (argc == 0u) return size == 0u;
	if (data == NULL || size == 0u || argc > size) return false;
	end = cursor + size;
	for (uint32_t i = 0u; i < argc; i++) {
		while (cursor < end && *cursor != 0u) cursor++;
		if (cursor == end) return false;
		cursor++;
	}
	return cursor == end;
}

syscall_status_t loader_start_program(struct loader_loaded_program* program, uint32_t argc, const void* argv_data,
                                      size_t argv_size, cap_id_t* out_thread_cap) {
	struct process_startup_info* startup;
	if (program == NULL || out_thread_cap == NULL || program->started ||
	    !argv_payload_valid(argc, argv_data, argv_size) || argv_size > THREAD_START_ARG_MAX_SIZE - sizeof(*startup))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_thread_cap     = CAP_ID_INVALID;
	size_t startup_size = sizeof(*startup) + argv_size;
	startup             = malloc(startup_size);
	if (startup == NULL) return SYSCALL_STATUS_FAILED;
	*startup = (struct process_startup_info){
		.size                 = (uint32_t)startup_size,
		.heap_base            = program->heap_base,
		.heap_size            = program->heap_size,
		.memory_allocator_cap = program->memory_allocator_cap,
		.serial_cap           = program->serial_cap,
		.init_cap             = program->init_cap,
		.argc                 = argc,
		.argv_offset          = argc == 0u ? 0u : (uint32_t)sizeof(*startup),
		.argv_size            = (uint32_t)argv_size,
	};
	if (argv_size != 0u) memcpy(startup + 1, argv_data, argv_size);
	syscall_status_t status = process_run(program->process_cap, program->entry, startup, startup_size, out_thread_cap);
	if (status == SYSCALL_STATUS_OK) program->started = true;
	free(startup);
	return status;
}

void loader_discard_program(struct loader_loaded_program* program) {
	if (program == NULL) return;
	if (program->load_cap != CAP_ID_INVALID) (void)cap_revoke(program->load_cap, 0u);
	while (program->mappings != NULL) {
		struct loader_mapping_authority* item = program->mappings;
		program->mappings                     = item->next;
		(void)mapping_unmap(item->cap);
		free(item);
	}
	if (program->process_cap != CAP_ID_INVALID) {
		(void)process_kill(program->process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		(void)process_wait(program->process_cap, NULL);
		(void)cap_drop(program->process_cap);
	}
	free(program);
}

syscall_status_t loader_prepare_program(cap_id_t blob_cap, const char* name, size_t name_size,
                                        struct loader_loaded_program** out_program) {
	struct elf64_image                image   = {0};
	struct loader_elf_load_plan       plan    = {0};
	struct loader_elf_segment_layout* layouts = NULL;
	struct loader_loaded_program*     program = NULL;
	struct process_create_response    created;
	struct process_info_response      process_info;
	struct address_space_info         space_info;
	char*                             process_name = NULL;
	syscall_status_t                  status;
	if (blob_cap == CAP_ID_INVALID || name == NULL || name_size == 0u || name_size == SIZE_MAX || out_program == NULL ||
	    memchr(name, '\0', name_size) != NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_program = NULL;
	status       = parse_status(elf64_image_parse(blob_cap, &image));
	if (status != SYSCALL_STATUS_OK) return status;
	program      = calloc(1u, sizeof(*program));
	process_name = malloc(name_size + 1u);
	if (program == NULL || process_name == NULL) goto failed;
	program->load_cap = program->process_cap = program->address_space_cap = CAP_ID_INVALID;
	program->process_id                                                   = PROCESS_PID_INVALID;
	program->init_cap = program->serial_cap = program->memory_allocator_cap = CAP_ID_INVALID;
	memcpy(process_name, name, name_size);
	process_name[name_size] = '\0';
	status                  = process_create(process_name, name_size + 1u, &created);
	if (status != SYSCALL_STATUS_OK) goto failed;
	program->process_cap       = created.process_cap;
	program->address_space_cap = created.address_space_cap;
	if (process_get_info(program->process_cap, &process_info) != SYSCALL_STATUS_OK ||
	    address_space_info(program->address_space_cap, &space_info) != SYSCALL_STATUS_OK ||
	    process_info.pid == PROCESS_PID_INVALID || space_info.minimum_mapping_size == 0u) {
		status = SYSCALL_STATUS_FAILED;
		goto failed;
	}
	program->process_id      = process_info.pid;
	program->mapping_granule = space_info.minimum_mapping_size;
	program->entry           = (uintptr_t)image.entry;
	layouts                  = calloc(image.segment_count, sizeof(*layouts));
	if (layouts == NULL) goto failed;
	for (size_t i = 0u; i < image.segment_count; i++)
		layouts[i] = (struct loader_elf_segment_layout){
			.vaddr  = image.segments[i].vaddr,
			.memsz  = image.segments[i].memsz,
			.access = segment_access(image.segments[i].flags),
		};
	status = plan_status(loader_elf_plan_create(layouts, image.segment_count, program->mapping_granule, &plan));
	if (status != SYSCALL_STATUS_OK || image.entry > UINTPTR_MAX ||
	    !loader_elf_entry_is_executable(layouts, image.segment_count, image.entry)) {
		if (status == SYSCALL_STATUS_OK) status = SYSCALL_STATUS_BAD_ARGUMENT;
		goto failed;
	}
	for (size_t i = 0u; i < plan.region_count; i++) {
		status = load_region(program, blob_cap, &image, &plan, &plan.regions[i]);
		if (status != SYSCALL_STATUS_OK) goto failed;
	}
	status = allocate_heap(program);
	if (status != SYSCALL_STATUS_OK || (status = delegate_runtime_caps(program)) != SYSCALL_STATUS_OK ||
	    (status = transfer_mappings(program)) != SYSCALL_STATUS_OK ||
	    (status = cap_drop(program->address_space_cap)) != SYSCALL_STATUS_OK)
		goto failed;
	program->address_space_cap = CAP_ID_INVALID;
	free(process_name);
	free(layouts);
	loader_elf_plan_deinit(&plan);
	elf64_image_deinit(&image);
	*out_program = program;
	return SYSCALL_STATUS_OK;
failed:
	free(process_name);
	free(layouts);
	loader_elf_plan_deinit(&plan);
	elf64_image_deinit(&image);
	loader_discard_program(program);
	return status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status;
}
