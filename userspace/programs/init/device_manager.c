#include "device_manager.h"

#include <base/kernel_resource.h>
#include <base/module.h>
#include <base/startup.h>
#include <protocol/device_manager.h>
#include <protocol/loader.h>
#include <runtime/init.h>
#include <runtime/program.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/kernel_resource.h>
#include <system/module.h>
#include <system/process.h>

#include "module_blob.h"
#include "registry.h"

#define DEVICE_MANAGER_MODULE_NAME "device_manager.elf"
#define DEVICE_MANAGER_PROCESS_NAME "device-manager"
#define DEVICE_MANAGER_LOADER_SERVICE "elf64"
#define DEVICE_MANAGER_LOADER_MAJOR 1u
#define DEVICE_MANAGER_LOADER_MINOR 0u

static bool device_manager_drop_capability(cap_id_t* capability, const char* description) {
	syscall_status_t status;

	if (capability == NULL || *capability == CAP_ID_INVALID) return true;
	status = cap_drop(*capability);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: device manager %s capability drop failed: %u\n", description, (unsigned)status);
		return false;
	}
	*capability = CAP_ID_INVALID;
	return true;
}

static syscall_status_t device_manager_load(const struct init_service_handle* loader, cap_id_t blob_cap,
                                            struct program_load_result* out_load) {
	struct {
		struct loader_v1_load_request request;
		char                          name[sizeof(DEVICE_MANAGER_PROCESS_NAME) - 1u];
	} payload = {
		.request =
			{
					  .header    = {.op = LOADER_V1_OP_LOAD},
					  .name_size = sizeof(DEVICE_MANAGER_PROCESS_NAME) - 1u,
					  },
	};
	struct loader_v1_load_response response       = {.load_cap = CAP_ID_INVALID, .process_id = PROCESS_PID_INVALID};
	cap_id_t                       delegated_blob = CAP_ID_INVALID;
	size_t                         response_size  = 0u;
	syscall_status_t               cleanup_status;
	syscall_status_t               status;

	if (loader == NULL || loader->capability == CAP_ID_INVALID || loader->owner == PROCESS_PID_INVALID ||
	    blob_cap == CAP_ID_INVALID || out_load == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_load = (struct program_load_result){
		.load_cap   = CAP_ID_INVALID,
		.process_id = PROCESS_PID_INVALID,
	};
	status = cap_delegate(blob_cap, loader->owner, LOADER_V1_BLOB_CAP_RIGHTS, &delegated_blob);
	if (status != SYSCALL_STATUS_OK) return status;
	payload.request.blob_cap = delegated_blob;
	memcpy(payload.name, DEVICE_MANAGER_PROCESS_NAME, sizeof(payload.name));
	status         = cap_call(loader->capability,
	                          &payload,
	                          sizeof(payload.request) + sizeof(payload.name),
	                          &response,
	                          sizeof(response),
	                          &response_size);
	cleanup_status = cap_revoke(delegated_blob, 0u);
	if (status == SYSCALL_STATUS_OK && cleanup_status != SYSCALL_STATUS_OK) status = cleanup_status;
	if (status != SYSCALL_STATUS_OK || response_size != sizeof(response) || response.load_cap == CAP_ID_INVALID ||
	    response.process_id == PROCESS_PID_INVALID) {
		if (response.load_cap != CAP_ID_INVALID) (void)program_cancel(response.load_cap);
		return status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status;
	}
	*out_load = (struct program_load_result){
		.load_cap   = response.load_cap,
		.process_id = response.process_id,
	};
	return SYSCALL_STATUS_OK;
}

static syscall_status_t device_manager_optional_resource(const struct init_state* init, enum kernel_resource_type type,
                                                         cap_id_t* out_cap) {
	syscall_status_t status;

	*out_cap = CAP_ID_INVALID;
	status   = kernel_resource_acquire(init->kernel_resources_cap, type, out_cap);
	return status == SYSCALL_STATUS_UNAVAILABLE ? SYSCALL_STATUS_OK : status;
}

enum device_manager_launch_result device_manager_launch(const struct init_state* init) {
	static const struct init_protocol_query loader_query = {
		.namespace_path = PROGRAM_LOADER_NAMESPACE,
		.protocol       = LOADER_PROTOCOL_NAME,
		.major          = DEVICE_MANAGER_LOADER_MAJOR,
		.minor          = DEVICE_MANAGER_LOADER_MINOR,
	};
	struct module_provider_resolve_response module = {.cap = CAP_ID_INVALID};
	struct init_service_handle              loader = {.capability = CAP_ID_INVALID};
	struct program_load_result              loaded = {
		.load_cap   = CAP_ID_INVALID,
		.process_id = PROCESS_PID_INVALID,
	};
	struct program_run_result running;
	struct self_info          self;
	cap_id_t                  acpi_cap    = CAP_ID_INVALID;
	cap_id_t                  blob_cap    = CAP_ID_INVALID;
	cap_id_t                  dt_cap      = CAP_ID_INVALID;
	cap_id_t                  modules_cap = CAP_ID_INVALID;
	enum init_registry_status registry_status;
	syscall_status_t          status;
	bool                      temporary_caps_released;

	if (init == NULL) return DEVICE_MANAGER_LAUNCH_FAILED;
	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) {
		printf("init: self process query for device manager failed: %u\n", (unsigned)status);
		return DEVICE_MANAGER_LAUNCH_FAILED;
	}
	registry_status = registry_acquire(self.pid, &loader_query, DEVICE_MANAGER_LOADER_SERVICE, &loader);
	if (registry_status == INIT_REGISTRY_NOT_FOUND) return DEVICE_MANAGER_LAUNCH_UNAVAILABLE;
	if (registry_status != INIT_REGISTRY_OK) {
		printf("init: device manager loader acquisition failed: %u\n", (unsigned)registry_status);
		return DEVICE_MANAGER_LAUNCH_FAILED;
	}
	status = kernel_resource_acquire(init->kernel_resources_cap, KERNEL_RESOURCE_TYPE_MODULES, &modules_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: device manager modules provider acquisition failed: %u\n", (unsigned)status);
		(void)device_manager_drop_capability(&loader.capability, "loader");
		return DEVICE_MANAGER_LAUNCH_FAILED;
	}
	status = module_resolve(modules_cap, DEVICE_MANAGER_MODULE_NAME, sizeof(DEVICE_MANAGER_MODULE_NAME), &module);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: device_manager.elf module resolution failed: %u\n", (unsigned)status);
	}
	temporary_caps_released = device_manager_drop_capability(&modules_cap, "modules provider");
	if (status == SYSCALL_STATUS_OK) {
		status = module_blob_create(module.cap, module.size, &blob_cap);
		if (status == SYSCALL_STATUS_OK) module.cap = CAP_ID_INVALID;
	}
	if (status == SYSCALL_STATUS_OK) status = device_manager_load(&loader, blob_cap, &loaded);
	if (module.cap != CAP_ID_INVALID && !device_manager_drop_capability(&module.cap, "module"))
		temporary_caps_released = false;
	if (!device_manager_drop_capability(&loader.capability, "loader")) temporary_caps_released = false;
	if (status != SYSCALL_STATUS_OK || !temporary_caps_released) {
		if (status != SYSCALL_STATUS_OK) printf("init: device manager load failed: %u\n", (unsigned)status);
		if (loaded.load_cap != CAP_ID_INVALID) (void)program_cancel(loaded.load_cap);
		return DEVICE_MANAGER_LAUNCH_FAILED;
	}
	status = device_manager_optional_resource(init, KERNEL_RESOURCE_TYPE_ACPI, &acpi_cap);
	if (status == SYSCALL_STATUS_OK)
		status = device_manager_optional_resource(init, KERNEL_RESOURCE_TYPE_DEVICE_TREE, &dt_cap);
	if (status == SYSCALL_STATUS_OK) {
		const struct program_capability_argument capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_COUNT] = {
			[PROCESS_STARTUP_CAP_STDIN] =
				{
											 .capability = CAP_ID_INVALID,
											 },
			[PROCESS_STARTUP_CAP_STDOUT] =
				{
											 .capability = init->serial_stream_cap,
											 .rights     = CAP_CALL | CAP_WRITE,
											 },
			[PROCESS_STARTUP_CAP_STDERR] =
				{
											 .capability = init->serial_stream_cap,
											 .rights     = CAP_CALL | CAP_WRITE,
											 },
			[PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_ACPI] =
				{
											 .capability = acpi_cap,
											 .rights     = CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE,
											 },
			[PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_DEVICE_TREE] =
				{
											 .capability = dt_cap,
											 .rights     = CAP_CALL | CAP_READ | CAP_DELEGATE,
											 },
		};
		status =
			program_run(&loaded, 0u, NULL, PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_COUNT, capv, &running);
	}
	if (!device_manager_drop_capability(&acpi_cap, "ACPI provider")) temporary_caps_released = false;
	if (!device_manager_drop_capability(&dt_cap, "Device Tree provider")) temporary_caps_released = false;
	if (status != SYSCALL_STATUS_OK) {
		printf("init: device manager start failed: %u\n", (unsigned)status);
		(void)program_cancel(loaded.load_cap);
		return DEVICE_MANAGER_LAUNCH_FAILED;
	}
	status = process_detach(running.process_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: device manager detach failed: %u\n", (unsigned)status);
	}
	if (!device_manager_drop_capability(&running.thread_cap, "thread")) temporary_caps_released = false;
	if (!device_manager_drop_capability(&running.process_cap, "process")) temporary_caps_released = false;
	return status == SYSCALL_STATUS_OK && temporary_caps_released ? DEVICE_MANAGER_LAUNCH_SUCCESS
	                                                              : DEVICE_MANAGER_LAUNCH_FAILED;
}
