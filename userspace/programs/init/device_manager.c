#include "device_manager.h"

#include <base/kernel_resource.h>
#include <base/startup.h>
#include <protocol/device_manager.h>
#include <protocol/loader.h>
#include <protocol/vfs.h>
#include <runtime/init.h>
#include <runtime/program.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/kernel_resource.h>
#include <system/process.h>

#include "arguments.h"
#include "registry.h"

#define DEVICE_MANAGER_FILE_PATH "/boot/device_manager.elf"
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

static syscall_status_t device_manager_load(const struct init_service_handle* loader, cap_id_t file_cap,
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
	cap_id_t                       delegated_file = CAP_ID_INVALID;
	size_t                         response_size  = 0u;
	syscall_status_t               cleanup_status;
	syscall_status_t               status;

	if (loader == NULL || loader->capability == CAP_ID_INVALID || loader->owner == PROCESS_PID_INVALID ||
	    file_cap == CAP_ID_INVALID || out_load == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_load = (struct program_load_result){
		.load_cap   = CAP_ID_INVALID,
		.process_id = PROCESS_PID_INVALID,
	};
	status = cap_delegate(file_cap, loader->owner, LOADER_V1_FILE_CAP_RIGHTS, &delegated_file);
	if (status != SYSCALL_STATUS_OK) return status;
	payload.request.file_cap = delegated_file;
	memcpy(payload.name, DEVICE_MANAGER_PROCESS_NAME, sizeof(payload.name));
	status         = cap_call(loader->capability,
	                          &payload,
	                          sizeof(payload.request) + sizeof(payload.name),
	                          &response,
	                          sizeof(response),
	                          &response_size);
	cleanup_status = cap_revoke(delegated_file, 0u);
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
	static const struct init_protocol_query vfs_query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle    loader     = {.capability = CAP_ID_INVALID};
	struct init_service_handle    vfs        = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle executable = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result open_result;
	struct program_load_result    loaded = {
		.load_cap   = CAP_ID_INVALID,
		.process_id = PROCESS_PID_INVALID,
	};
	struct program_run_result running;
	struct self_info          self;
	cap_id_t                  acpi_cap       = CAP_ID_INVALID;
	cap_id_t                  dt_cap         = CAP_ID_INVALID;
	cap_id_t                  interrupts_cap = CAP_ID_INVALID;
	cap_id_t                  io_ports_cap   = CAP_ID_INVALID;
	const char**              argv           = NULL;
	enum init_registry_status registry_status;
	syscall_status_t          status;
	bool                      temporary_caps_released = true;

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
	registry_status = registry_acquire(self.pid, &vfs_query, VFS_SERVICE_NAME, &vfs);
	if (registry_status == INIT_REGISTRY_NOT_FOUND) {
		if (!device_manager_drop_capability(&loader.capability, "loader")) return DEVICE_MANAGER_LAUNCH_FAILED;
		return DEVICE_MANAGER_LAUNCH_UNAVAILABLE;
	}
	if (registry_status != INIT_REGISTRY_OK) {
		printf("init: device manager VFS acquisition failed: %u\n", (unsigned)registry_status);
		(void)device_manager_drop_capability(&loader.capability, "loader");
		return DEVICE_MANAGER_LAUNCH_FAILED;
	}
	open_result = vfs_open(vfs.capability,
	                       DEVICE_MANAGER_FILE_PATH,
	                       sizeof(DEVICE_MANAGER_FILE_PATH) - 1u,
	                       CAP_READ | CAP_DELEGATE,
	                       &executable);
	if (open_result.transport_status != SYSCALL_STATUS_OK || open_result.status != FILESYSTEM_STATUS_OK ||
	    executable.info.type != FILESYSTEM_NODE_FILE) {
		printf("init: device manager file open failed: transport=%u status=%u\n",
		       (unsigned)open_result.transport_status,
		       (unsigned)open_result.status);
		status =
			open_result.transport_status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : open_result.transport_status;
	}
	else {
		status = device_manager_load(&loader, executable.capability, &loaded);
	}
	if (!device_manager_drop_capability(&executable.capability, "executable file")) temporary_caps_released = false;
	if (!device_manager_drop_capability(&vfs.capability, "VFS")) temporary_caps_released = false;
	if (!device_manager_drop_capability(&loader.capability, "loader")) temporary_caps_released = false;
	if (status != SYSCALL_STATUS_OK || !temporary_caps_released) {
		if (status != SYSCALL_STATUS_OK) printf("init: device manager load failed: %u\n", (unsigned)status);
		if (loaded.load_cap != CAP_ID_INVALID) (void)program_cancel(loaded.load_cap);
		return DEVICE_MANAGER_LAUNCH_FAILED;
	}
	status = device_manager_optional_resource(init, KERNEL_RESOURCE_TYPE_ACPI, &acpi_cap);
	if (status == SYSCALL_STATUS_OK)
		status = device_manager_optional_resource(init, KERNEL_RESOURCE_TYPE_DEVICE_TREE, &dt_cap);
	if (status == SYSCALL_STATUS_OK)
		status = kernel_resource_acquire(init->kernel_resources_cap, KERNEL_RESOURCE_TYPE_INTERRUPTS, &interrupts_cap);
#if defined(__x86_64__)
	if (status == SYSCALL_STATUS_OK)
		status = device_manager_optional_resource(init, KERNEL_RESOURCE_TYPE_IO_PORTS, &io_ports_cap);
#endif
	if (status == SYSCALL_STATUS_OK && !init_arguments_vector(init, &argv)) status = SYSCALL_STATUS_FAILED;
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
			[PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_MEMORY_ALLOCATOR] =
				{
											 .capability = init->memory_allocator_cap,
											 .rights     = DEVICE_MANAGER_MEMORY_ALLOCATOR_CAP_RIGHTS,
											 },
			[PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_INTERRUPTS] =
				{
											 .capability = interrupts_cap,
											 .rights     = DEVICE_MANAGER_INTERRUPTS_CAP_RIGHTS,
											 },
			[PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_IO_PORTS] =
				{
											 .capability = io_ports_cap,
											 .rights     = io_ports_cap == CAP_ID_INVALID ? 0u : DEVICE_PARSER_IO_PORTS_CAP_RIGHTS,
											 },
		};
		status = program_run(
			&loaded, init->argc, argv, PROCESS_STARTUP_CAP_COUNT + DEVICE_MANAGER_CAPABILITY_COUNT, capv, &running);
	}
	free(argv);
	if (!device_manager_drop_capability(&acpi_cap, "ACPI provider")) temporary_caps_released = false;
	if (!device_manager_drop_capability(&dt_cap, "Device Tree provider")) temporary_caps_released = false;
	if (!device_manager_drop_capability(&interrupts_cap, "interrupts")) temporary_caps_released = false;
	if (!device_manager_drop_capability(&io_ports_cap, "I/O ports")) temporary_caps_released = false;
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
