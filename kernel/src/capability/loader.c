#include <base/cap.h>
#include <base/loader.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/process.h>
#include <kernel/capability.h>
#include <kernel/elf_loader.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "address_space.h"
#include "boot_module.h"
#include "process.h"

static cap_object_id_t loader_object_id = CAP_OBJECT_ID_INVALID;

static const char* module_process_name(const struct kernel_boot_module* module) {
	const char* basename;

	if (module->name != NULL && module->name[0] != '\0') return module->name;
	if (module->path == NULL) return "module";

	basename = module->path;
	for (const char* cursor = module->path; *cursor != '\0'; cursor++) {
		if (*cursor == '/' || *cursor == '\\') basename = cursor + 1;
	}
	return basename[0] != '\0' ? basename : "module";
}

static syscall_result_t loader_handler(const struct cap_request* req) {
	struct loader_load_request       request;
	struct loader_load_response      response;
	const struct kernel_boot_module* module;
	struct kernel_elf_process        loaded = {0};
	enum kernel_elf_load_result      load_result;
	syscall_result_t                 result;

	if (req->request == NULL || req->request_size < sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&request, req->request, sizeof(request));

	result = kernel_capability_boot_module_get(request.module_cap, req->caller, CAP_READ, &module);
	if (result.status != SYSCALL_STATUS_OK) return result;

	load_result = kernel_elf_load_process(module, module_process_name(module), &loaded);
	if (load_result != KERNEL_ELF_LOAD_OK) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)load_result);
	}

	response = (struct loader_load_response){
		.process_cap =
			kernel_process_grant(loaded.process,
	                             req->caller,
	                             CAP_CALL | CAP_READ | CAP_WAIT | CAP_MANAGE | CAP_DESTROY | CAP_EXEC | CAP_DELEGATE,
	                             NULL),
		.address_space_cap = CAP_ID_INVALID,
		.entry             = loaded.entry,
		.heap_base         = loaded.heap_base,
		.heap_page_count   = loaded.heap_page_count,
	};
	if (response.process_cap == CAP_ID_INVALID) {
		(void)process_destroy(loaded.process);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	response.address_space_cap =
		kernel_address_space_grant(loaded.process, req->caller, CAP_CALL | CAP_MAP | CAP_READ | CAP_DELEGATE, NULL);
	if (response.address_space_cap == CAP_ID_INVALID) {
		(void)cap_destroy_by_id(response.process_cap);
		(void)process_destroy(loaded.process);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) {
		(void)cap_destroy_by_id(response.address_space_cap);
		(void)cap_destroy_by_id(response.process_cap);
		(void)process_destroy(loaded.process);
	}
	return result;
}

void kernel_capability_loader_init(void) {
	loader_object_id = cap_object_create_kernel(0u, loader_handler, NULL);
}

cap_id_t kernel_capability_loader_grant(process_id_t recipient) {
	if (loader_object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
	return cap_create(loader_object_id, recipient, CAP_CALL | CAP_DELEGATE, NULL, NULL);
}
