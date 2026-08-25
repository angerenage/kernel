#include "load.h"

#include <base/heap.h>
#include <base/math.h>
#include <base/startup.h>
#include <base/thread.h>
#include <base/vmm.h>
#include <runtime/blob.h>
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

#define LOAD_COPY_CHUNK_SIZE 4096u

static syscall_status_t parse_status(enum elf64_parse_result result) {
	switch (result) {
	case ELF64_PARSE_OK:
		return SYSCALL_STATUS_OK;
	case ELF64_PARSE_INVALID_ARGUMENT:
	case ELF64_PARSE_BAD_FORMAT:
		return SYSCALL_STATUS_BAD_ARGUMENT;
	case ELF64_PARSE_UNSUPPORTED:
		return SYSCALL_STATUS_UNAVAILABLE;
	case ELF64_PARSE_IO_ERROR:
	case ELF64_PARSE_NO_MEMORY:
	default:
		return SYSCALL_STATUS_FAILED;
	}
}

static vmm_prot_t segment_prot(uint32_t flags) {
	vmm_prot_t prot = VMM_PROT_NONE;
	if ((flags & ELF64_SEGMENT_READ) != 0u) prot |= VMM_PROT_READ;
	if ((flags & ELF64_SEGMENT_WRITE) != 0u) prot |= VMM_PROT_WRITE;
	if ((flags & ELF64_SEGMENT_EXEC) != 0u) prot |= VMM_PROT_EXEC;
	return prot;
}

static syscall_status_t plan_status(enum loader_elf_plan_result result) {
	switch (result) {
	case LOADER_ELF_PLAN_OK:
		return SYSCALL_STATUS_OK;
	case LOADER_ELF_PLAN_INVALID_ARGUMENT:
		return SYSCALL_STATUS_BAD_ARGUMENT;
	case LOADER_ELF_PLAN_BAD_LAYOUT:
		return SYSCALL_STATUS_UNAVAILABLE;
	case LOADER_ELF_PLAN_NO_MEMORY:
	default:
		return SYSCALL_STATUS_FAILED;
	}
}

static bool segment_in_region(const struct elf64_segment* segment, const struct loader_elf_load_region* region) {
	uint64_t region_size;
	uint64_t region_end;
	uint64_t segment_end;
	if (segment->memsz == 0u || mul_overflow_u64(region->page_count, VMM_PAGE_SIZE, &region_size) ||
	    add_overflow_u64(region->virtual_base, region_size, &region_end) ||
	    add_overflow_u64(segment->vaddr, segment->memsz, &segment_end))
		return false;
	return segment->vaddr >= region->virtual_base && segment_end <= region_end;
}

static syscall_status_t populate_region(cap_id_t blob_cap, cap_id_t memory_cap, const struct elf64_image* image,
                                        const struct loader_elf_load_region* region) {
	uint8_t buffer[LOAD_COPY_CHUNK_SIZE];
	for (size_t i = 0u; i < image->segment_count; i++) {
		const struct elf64_segment* segment = &image->segments[i];
		if (!segment_in_region(segment, region) || segment->filesz == 0u) continue;
		for (uint64_t copied = 0u; copied < segment->filesz;) {
			uint64_t remaining = segment->filesz - copied;
			size_t   chunk     = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
			uint64_t source_offset;
			uint64_t memory_offset;
			if (add_overflow_u64(segment->offset, copied, &source_offset) ||
			    add_overflow_u64(segment->vaddr - region->virtual_base, copied, &memory_offset) ||
			    memory_offset > SIZE_MAX)
				return SYSCALL_STATUS_BAD_ARGUMENT;
			syscall_status_t status = blob_read(blob_cap, source_offset, buffer, chunk);
			if (status != SYSCALL_STATUS_OK) return status;
			status = memory_write(memory_cap, (size_t)memory_offset, buffer, chunk);
			if (status != SYSCALL_STATUS_OK) return status;
			copied += chunk;
		}
	}
	return SYSCALL_STATUS_OK;
}

static syscall_status_t load_region(struct loader_loaded_program* program, cap_id_t blob_cap,
                                    const struct elf64_image* image, const struct loader_elf_load_plan* plan,
                                    const struct loader_elf_load_region* region) {
	cap_id_t         memory_cap = CAP_ID_INVALID;
	syscall_status_t status;
	if (region->virtual_base > UINTPTR_MAX) return SYSCALL_STATUS_BAD_ARGUMENT;
	const struct memory_create_params create_params = {.page_count = region->page_count};
	status                                          = memory_create(&create_params, &memory_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	status = populate_region(blob_cap, memory_cap, image, region);
	if (status != SYSCALL_STATUS_OK) goto cleanup;

	for (size_t i = 0u; i < region->run_count; i++) {
		const struct loader_elf_load_run* run = &plan->runs[region->first_run + i];
		uint64_t                          run_offset;
		uint64_t                          run_base;
		struct address_space_map_result   mapped = {.mapping_cap = CAP_ID_INVALID};
		if (mul_overflow_u64(run->object_page_offset, VMM_PAGE_SIZE, &run_offset) ||
		    add_overflow_u64(region->virtual_base, run_offset, &run_base) || run_base > UINTPTR_MAX) {
			status = SYSCALL_STATUS_BAD_ARGUMENT;
			goto cleanup;
		}
		const struct memory_map_params params = {
			.memory_page_offset = run->object_page_offset,
			.page_count         = run->page_count,
			.address            = (uintptr_t)run_base,
			.align_pages        = 1u,
			.guard_pages        = 0u,
			.prot               = run->prot,
		};
		status = address_space_map(program->address_space_cap, memory_cap, &params, &mapped);
		if (status != SYSCALL_STATUS_OK) goto cleanup;
		status = cap_drop(mapped.mapping_cap);
		if (status != SYSCALL_STATUS_OK) {
			(void)mapping_unmap(mapped.mapping_cap);
			goto cleanup;
		}
	}
	status = cap_drop(memory_cap);
	if (status == SYSCALL_STATUS_OK) memory_cap = CAP_ID_INVALID;
	return status;

cleanup:
	if (memory_cap != CAP_ID_INVALID) (void)cap_drop(memory_cap);
	return status;
}

static syscall_status_t allocate_heap(struct loader_loaded_program* program) {
	cap_id_t                        memory_cap = CAP_ID_INVALID;
	struct address_space_map_result mapped     = {.mapping_cap = CAP_ID_INVALID};
	syscall_status_t                status;

	const struct memory_create_params create_params = {.page_count = HEAP_DEFAULT_GROW_PAGES};
	status                                          = memory_create(&create_params, &memory_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	const struct memory_map_params params = {
		.page_count = HEAP_DEFAULT_GROW_PAGES, .align_pages = 1u, .prot = VMM_PROT_READ | VMM_PROT_WRITE};
	status = address_space_map(program->address_space_cap, memory_cap, &params, &mapped);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	if (mapped.mapping.base == NULL || mapped.mapping.page_count != HEAP_DEFAULT_GROW_PAGES) {
		status = SYSCALL_STATUS_FAILED;
		goto cleanup;
	}
	status = cap_drop(memory_cap);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	memory_cap = CAP_ID_INVALID;
	status     = cap_drop(mapped.mapping_cap);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	mapped.mapping_cap       = CAP_ID_INVALID;
	program->heap_base       = (uintptr_t)mapped.mapping.base;
	program->heap_page_count = mapped.mapping.page_count;
	return SYSCALL_STATUS_OK;

cleanup:
	if (mapped.mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(mapped.mapping_cap);
	if (memory_cap != CAP_ID_INVALID) (void)cap_drop(memory_cap);
	return status;
}

static syscall_status_t delegate_runtime_caps(struct loader_loaded_program* program) {
	syscall_status_t status;

	if (init_cap_id == CAP_ID_INVALID || serial_cap_id == CAP_ID_INVALID) return SYSCALL_STATUS_UNAVAILABLE;
	status = cap_delegate_peer(init_cap_id, program->process_id, CAP_CALL, &program->init_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	return cap_delegate_peer(serial_cap_id, program->process_id, CAP_CALL | CAP_WRITE, &program->serial_cap);
}

static bool argv_payload_valid(uint32_t argc, const void* argv_data, size_t argv_size) {
	const uint8_t* cursor = argv_data;
	const uint8_t* end;

	if (argc == 0u) return argv_size == 0u;
	if (argv_data == NULL || argv_size == 0u || argc > argv_size) return false;
	end = cursor + argv_size;
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
	size_t                       startup_size;
	syscall_status_t             status;

	if (program == NULL || out_thread_cap == NULL || program->started ||
	    !argv_payload_valid(argc, argv_data, argv_size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_thread_cap = CAP_ID_INVALID;
	if (argv_size > THREAD_START_ARG_MAX_SIZE - sizeof(*startup)) return SYSCALL_STATUS_BAD_ARGUMENT;
	startup_size = sizeof(*startup) + argv_size;
	startup      = malloc(startup_size);
	if (startup == NULL) return SYSCALL_STATUS_FAILED;

	*startup = (struct process_startup_info){
		.size            = (uint32_t)startup_size,
		.heap_base       = program->heap_base,
		.heap_page_count = program->heap_page_count,
		.page_size       = VMM_PAGE_SIZE,
		.serial_cap      = program->serial_cap,
		.init_cap        = program->init_cap,
		.argc            = argc,
		.argv_offset     = argc == 0u ? 0u : (uint32_t)sizeof(*startup),
		.argv_size       = (uint32_t)argv_size,
	};
	if (argv_size != 0u) memcpy(startup + 1, argv_data, argv_size);

	status           = process_run(program->process_cap, program->entry, startup, startup_size, out_thread_cap);
	program->started = true;
	free(startup);
	return status;
}

void loader_discard_program(struct loader_loaded_program* program) {
	if (program == NULL) return;
	if (program->load_cap != CAP_ID_INVALID) (void)cap_revoke(program->load_cap, 0u);
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
	struct loader_loaded_program*     program;
	struct process_create_response    created;
	struct process_info_response      process_info;
	char*                             process_name = NULL;
	syscall_status_t                  status;

	if (blob_cap == CAP_ID_INVALID || name == NULL || name_size == 0u || name_size == SIZE_MAX || out_program == NULL ||
	    memchr(name, '\0', name_size) != NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_program = NULL;
	status       = parse_status(elf64_image_parse(blob_cap, &image));
	if (status != SYSCALL_STATUS_OK) return status;
	if (image.segment_count > SIZE_MAX / sizeof(*layouts)) {
		status = SYSCALL_STATUS_BAD_ARGUMENT;
		goto fail_image;
	}
	layouts = calloc(image.segment_count, sizeof(*layouts));
	if (layouts == NULL) {
		status = SYSCALL_STATUS_FAILED;
		goto fail_image;
	}
	for (size_t i = 0u; i < image.segment_count; i++) {
		layouts[i] = (struct loader_elf_segment_layout){
			.vaddr = image.segments[i].vaddr,
			.memsz = image.segments[i].memsz,
			.prot  = segment_prot(image.segments[i].flags),
		};
	}
	status = plan_status(loader_elf_plan_create(layouts, image.segment_count, &plan));
	if (status == SYSCALL_STATUS_OK &&
	    (image.entry > UINTPTR_MAX || !loader_elf_entry_is_executable(layouts, image.segment_count, image.entry)))
		status = SYSCALL_STATUS_BAD_ARGUMENT;
	free(layouts);
	layouts = NULL;
	if (status != SYSCALL_STATUS_OK) goto fail_image;

	program = calloc(1u, sizeof(*program));
	if (program == NULL) {
		loader_elf_plan_deinit(&plan);
		elf64_image_deinit(&image);
		return SYSCALL_STATUS_FAILED;
	}
	program->load_cap          = CAP_ID_INVALID;
	program->process_cap       = CAP_ID_INVALID;
	program->address_space_cap = CAP_ID_INVALID;
	program->process_id        = PROCESS_PID_INVALID;
	program->init_cap          = CAP_ID_INVALID;
	program->serial_cap        = CAP_ID_INVALID;
	process_name               = malloc(name_size + 1u);
	if (process_name == NULL) {
		status = SYSCALL_STATUS_FAILED;
		goto fail;
	}
	memcpy(process_name, name, name_size);
	process_name[name_size] = '\0';
	status                  = process_create(process_name, name_size + 1u, &created);
	free(process_name);
	process_name = NULL;
	if (status != SYSCALL_STATUS_OK) goto fail;
	program->process_cap       = created.process_cap;
	program->address_space_cap = created.address_space_cap;
	status                     = process_get_info(program->process_cap, &process_info);
	if (status != SYSCALL_STATUS_OK || process_info.pid == PROCESS_PID_INVALID) {
		status = status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status;
		goto fail;
	}
	program->process_id = process_info.pid;
	program->entry      = (uintptr_t)image.entry;

	for (size_t i = 0u; i < plan.region_count; i++) {
		status = load_region(program, blob_cap, &image, &plan, &plan.regions[i]);
		if (status != SYSCALL_STATUS_OK) goto fail;
	}
	status = allocate_heap(program);
	if (status != SYSCALL_STATUS_OK) goto fail;
	status = delegate_runtime_caps(program);
	if (status != SYSCALL_STATUS_OK) goto fail;
	status = cap_drop(program->address_space_cap);
	if (status != SYSCALL_STATUS_OK) goto fail;
	program->address_space_cap = CAP_ID_INVALID;

	loader_elf_plan_deinit(&plan);
	elf64_image_deinit(&image);
	*out_program = program;
	return SYSCALL_STATUS_OK;

fail:
	free(process_name);
	loader_elf_plan_deinit(&plan);
	elf64_image_deinit(&image);
	loader_discard_program(program);
	return status;

fail_image:
	free(layouts);
	loader_elf_plan_deinit(&plan);
	elf64_image_deinit(&image);
	return status;
}
