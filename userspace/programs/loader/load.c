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

static bool remember_allocation(struct loader_loaded_program* program, cap_id_t allocation_cap) {
	if (program == NULL || allocation_cap == CAP_ID_INVALID || program->allocation_caps == NULL) return false;
	program->allocation_caps[program->allocation_count++] = allocation_cap;
	return true;
}

static syscall_status_t load_segment(struct loader_loaded_program* program, cap_id_t blob_cap,
                                     const struct elf64_segment* segment) {
	uint8_t          buffer[LOAD_COPY_CHUNK_SIZE];
	uint64_t         page_base;
	uint64_t         page_offset;
	uint64_t         span;
	uint64_t         map_size;
	size_t           page_count;
	vmm_prot_t       final_prot;
	vmm_prot_t       load_prot;
	cap_id_t         allocation_cap = CAP_ID_INVALID;
	cap_id_t         mapping_cap    = CAP_ID_INVALID;
	syscall_status_t status;

	if (segment->memsz == 0u) return SYSCALL_STATUS_OK;
	page_base   = align_down_u64(segment->vaddr, VMM_PAGE_SIZE);
	page_offset = segment->vaddr - page_base;
	if (add_overflow_u64(page_offset, segment->memsz, &span) || !align_up_u64(span, VMM_PAGE_SIZE, &map_size) ||
	    map_size == 0u || map_size / VMM_PAGE_SIZE > SIZE_MAX)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	page_count = (size_t)(map_size / VMM_PAGE_SIZE);
	if (page_base > UINTPTR_MAX || page_offset > UINTPTR_MAX) return SYSCALL_STATUS_BAD_ARGUMENT;

	final_prot = segment_prot(segment->flags);
	load_prot  = final_prot | VMM_PROT_READ | VMM_PROT_WRITE;
	status     = memory_allocate(page_count, load_prot, &allocation_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	if (!remember_allocation(program, allocation_cap)) {
		(void)allocation_free(allocation_cap);
		return SYSCALL_STATUS_FAILED;
	}

	for (uint64_t copied = 0u; copied < segment->filesz;) {
		uint64_t remaining = segment->filesz - copied;
		size_t   chunk     = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
		uint64_t source_offset;
		uint64_t allocation_offset;

		if (add_overflow_u64(segment->offset, copied, &source_offset) ||
		    add_overflow_u64(page_offset, copied, &allocation_offset) || allocation_offset > UINTPTR_MAX)
			return SYSCALL_STATUS_BAD_ARGUMENT;
		status = blob_read(blob_cap, source_offset, buffer, chunk);
		if (status != SYSCALL_STATUS_OK) return status;
		status = allocation_write(allocation_cap, (uintptr_t)allocation_offset, buffer, chunk);
		if (status != SYSCALL_STATUS_OK) return status;
		copied += chunk;
	}

	status = address_space_map_at(program->address_space_cap, allocation_cap, (uintptr_t)page_base, &mapping_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	if (final_prot != load_prot) {
		status = mapping_protect(mapping_cap, final_prot);
		if (status != SYSCALL_STATUS_OK) {
			(void)cap_drop(mapping_cap);
			return status;
		}
	}
	return cap_drop(mapping_cap);
}

static syscall_status_t allocate_heap(struct loader_loaded_program* program) {
	cap_id_t         allocation_cap = CAP_ID_INVALID;
	cap_id_t         mapping_cap    = CAP_ID_INVALID;
	struct vmm_info  mapping;
	syscall_status_t status;

	status = memory_allocate(HEAP_DEFAULT_GROW_PAGES, VMM_PROT_READ | VMM_PROT_WRITE, &allocation_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	if (!remember_allocation(program, allocation_cap)) {
		(void)allocation_free(allocation_cap);
		return SYSCALL_STATUS_FAILED;
	}
	status = address_space_map(program->address_space_cap, allocation_cap, &mapping_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	status = mapping_get_info(mapping_cap, &mapping);
	if (status != SYSCALL_STATUS_OK || mapping.base == NULL || mapping.page_count != HEAP_DEFAULT_GROW_PAGES) {
		(void)cap_drop(mapping_cap);
		return status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status;
	}
	status = cap_drop(mapping_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	program->heap_base       = (uintptr_t)mapping.base;
	program->heap_page_count = mapping.page_count;
	return SYSCALL_STATUS_OK;
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
	}
	for (size_t i = 0u; i < program->allocation_count; i++) {
		if (program->allocation_caps[i] != CAP_ID_INVALID) (void)allocation_free(program->allocation_caps[i]);
	}
	free(program->allocation_caps);
	free(program);
}

syscall_status_t loader_prepare_program(cap_id_t blob_cap, const char* name, size_t name_size,
                                        struct loader_loaded_program** out_program) {
	struct elf64_image             image = {0};
	struct loader_loaded_program*  program;
	struct process_create_response created;
	struct process_info_response   process_info;
	char*                          process_name = NULL;
	syscall_status_t               status;

	if (blob_cap == CAP_ID_INVALID || name == NULL || name_size == 0u || name_size == SIZE_MAX || out_program == NULL ||
	    memchr(name, '\0', name_size) != NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_program = NULL;
	status       = parse_status(elf64_image_parse(blob_cap, &image));
	if (status != SYSCALL_STATUS_OK) return status;

	program = calloc(1u, sizeof(*program));
	if (program == NULL) {
		elf64_image_deinit(&image);
		return SYSCALL_STATUS_FAILED;
	}
	program->load_cap          = CAP_ID_INVALID;
	program->process_cap       = CAP_ID_INVALID;
	program->address_space_cap = CAP_ID_INVALID;
	program->process_id        = PROCESS_PID_INVALID;
	program->init_cap          = CAP_ID_INVALID;
	program->serial_cap        = CAP_ID_INVALID;
	program->allocation_caps   = calloc(image.segment_count + 1u, sizeof(*program->allocation_caps));
	if (program->allocation_caps == NULL) {
		elf64_image_deinit(&image);
		free(program);
		return SYSCALL_STATUS_FAILED;
	}

	process_name = malloc(name_size + 1u);
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

	for (size_t i = 0u; i < image.segment_count; i++) {
		status = load_segment(program, blob_cap, &image.segments[i]);
		if (status != SYSCALL_STATUS_OK) goto fail;
	}
	status = allocate_heap(program);
	if (status != SYSCALL_STATUS_OK) goto fail;
	status = delegate_runtime_caps(program);
	if (status != SYSCALL_STATUS_OK) goto fail;
	status = cap_drop(program->address_space_cap);
	if (status != SYSCALL_STATUS_OK) goto fail;
	program->address_space_cap = CAP_ID_INVALID;

	elf64_image_deinit(&image);
	*out_program = program;
	return SYSCALL_STATUS_OK;

fail:
	free(process_name);
	elf64_image_deinit(&image);
	loader_discard_program(program);
	return status;
}
