#include "bootstrap.h"

#include <base/startup.h>
#include <protocol/device_manager.h>
#include <protocol/vfs.h>
#include <runtime/init.h>
#include <runtime/program.h>
#include <runtime/vfs.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/process.h>

#define DEVICE_MANAGER_LOADER_SERVICE "elf64"

struct parser_description {
	const char*  path;
	const char*  process_name;
	const char*  display_name;
	cap_rights_t firmware_rights;
};

static bool drop_capability(cap_id_t* capability, const char* description) {
	syscall_status_t status;

	if (capability == NULL || *capability == CAP_ID_INVALID) return true;
	status = cap_drop(*capability);
	if (status != SYSCALL_STATUS_OK) {
		printf("device-manager: %s capability drop failed: %u\n", description, (unsigned)status);
		return false;
	}
	*capability = CAP_ID_INVALID;
	return true;
}

static bool parser_description_get(enum device_manager_firmware_source source, struct parser_description* out) {
	if (out == NULL) return false;
	switch (source) {
	case DEVICE_MANAGER_FIRMWARE_SOURCE_DEVICE_TREE:
		*out = (struct parser_description){
			.path            = "/boot/dt_parser.elf",
			.process_name    = "dt-parser",
			.display_name    = "Device Tree parser",
			.firmware_rights = CAP_CALL | CAP_READ,
		};
		return true;
	case DEVICE_MANAGER_FIRMWARE_SOURCE_ACPI:
		*out = (struct parser_description){
			.path            = "/boot/acpi_parser.elf",
			.process_name    = "acpi-parser",
			.display_name    = "ACPI parser",
			.firmware_rights = CAP_CALL | CAP_READ | CAP_MANAGE,
		};
		return true;
	default:
		return false;
	}
}

bool device_manager_parser_launch(struct device_server* server, enum device_manager_firmware_source source,
                                  cap_id_t* firmware_cap, cap_id_t memory_allocator_cap, cap_id_t dma_cap,
                                  cap_id_t interrupts_cap, cap_id_t io_ports_cap) {
	static const struct init_protocol_query vfs_query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct parser_description     parser;
	struct init_service_handle    vfs        = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle executable = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result open_result;
	struct program_load_result    loaded = {
		.load_cap   = CAP_ID_INVALID,
		.process_id = PROCESS_PID_INVALID,
	};
	struct program_run_result running = {
		.process_cap = CAP_ID_INVALID,
		.thread_cap  = CAP_ID_INVALID,
	};
	struct init_call_result acquire_result;
	syscall_status_t        status     = SYSCALL_STATUS_FAILED;
	bool                    cleanup_ok = true;

	if (server == NULL || firmware_cap == NULL || *firmware_cap == CAP_ID_INVALID ||
	    memory_allocator_cap == CAP_ID_INVALID || interrupts_cap == CAP_ID_INVALID ||
	    server->root_cap == CAP_ID_INVALID || !server->root_published || !parser_description_get(source, &parser))
		return false;
	acquire_result = init_acquire(&vfs_query, VFS_SERVICE_NAME, &vfs);
	if (acquire_result.transport_status != SYSCALL_STATUS_OK || acquire_result.status != INIT_REGISTRY_OK) {
		printf("device-manager: VFS acquisition for %s failed: transport=%u status=%u\n",
		       parser.display_name,
		       (unsigned)acquire_result.transport_status,
		       (unsigned)acquire_result.status);
		goto cleanup;
	}
	open_result = vfs_open(vfs.capability, parser.path, strlen(parser.path), CAP_READ | CAP_DELEGATE, &executable);
	if (open_result.transport_status != SYSCALL_STATUS_OK || open_result.status != FILESYSTEM_STATUS_OK ||
	    executable.info.type != FILESYSTEM_NODE_FILE) {
		printf("device-manager: %s executable open failed: transport=%u status=%u\n",
		       parser.display_name,
		       (unsigned)open_result.transport_status,
		       (unsigned)open_result.status);
		goto cleanup;
	}
	status = program_load(DEVICE_MANAGER_LOADER_SERVICE,
	                      executable.capability,
	                      parser.process_name,
	                      strlen(parser.process_name),
	                      &loaded);
	if (status != SYSCALL_STATUS_OK) {
		printf("device-manager: %s load failed: %u\n", parser.display_name, (unsigned)status);
		goto cleanup;
	}
	{
		struct program_capability_argument capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_COUNT] = {0};

		capv[PROCESS_STARTUP_CAP_STDIN].capability                          = CAP_ID_INVALID;
		capv[PROCESS_STARTUP_CAP_STDOUT].capability                         = CAP_ID_INVALID;
		capv[PROCESS_STARTUP_CAP_STDERR].capability                         = CAP_ID_INVALID;
		capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_FIRMWARE] = (struct program_capability_argument){
			.capability = *firmware_cap,
			.rights     = parser.firmware_rights,
		};
		capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_DEVICE_ROOT] = (struct program_capability_argument){
			.capability = server->root_cap,
			.rights     = DEVICE_PARSER_ROOT_CAP_RIGHTS,
		};
		capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_MEMORY_ALLOCATOR] =
			(struct program_capability_argument){
				.capability = memory_allocator_cap,
				.rights     = DEVICE_PARSER_MEMORY_ALLOCATOR_CAP_RIGHTS,
		};
		capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_DMA] = (struct program_capability_argument){
			.capability = dma_cap,
			.rights     = dma_cap == CAP_ID_INVALID ? 0u : DEVICE_PARSER_DMA_CAP_RIGHTS,
		};
		capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_INTERRUPTS] = (struct program_capability_argument){
			.capability = interrupts_cap,
			.rights     = DEVICE_PARSER_INTERRUPTS_CAP_RIGHTS,
		};
		if (source == DEVICE_MANAGER_FIRMWARE_SOURCE_ACPI && io_ports_cap != CAP_ID_INVALID)
			capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_IO_PORTS] = (struct program_capability_argument){
				.capability = io_ports_cap,
				.rights     = DEVICE_PARSER_IO_PORTS_CAP_RIGHTS,
			};
		status =
			program_run(&loaded, 0u, NULL, PROCESS_STARTUP_CAP_COUNT + DEVICE_PARSER_CAPABILITY_COUNT, capv, &running);
	}
	if (status != SYSCALL_STATUS_OK) {
		printf("device-manager: %s start failed: %u\n", parser.display_name, (unsigned)status);
		goto cleanup;
	}
	loaded.load_cap = CAP_ID_INVALID;
	if (!drop_capability(firmware_cap, "firmware source")) cleanup_ok = false;
	if (!drop_capability(&server->root_cap, "root-construction authority")) cleanup_ok = false;
	status = process_detach(running.process_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("device-manager: %s detach failed: %u\n", parser.display_name, (unsigned)status);
		cleanup_ok = false;
	}
	if (!drop_capability(&running.thread_cap, "parser thread")) cleanup_ok = false;
	if (!drop_capability(&running.process_cap, "parser process")) cleanup_ok = false;
	if (cleanup_ok) printf("device-manager: launched %s\n", parser.display_name);

cleanup:
	if (executable.capability != CAP_ID_INVALID && !drop_capability(&executable.capability, "parser executable"))
		cleanup_ok = false;
	if (vfs.capability != CAP_ID_INVALID && !drop_capability(&vfs.capability, "VFS")) cleanup_ok = false;
	if (running.process_cap == CAP_ID_INVALID && loaded.load_cap != CAP_ID_INVALID) {
		if (program_cancel(loaded.load_cap) != SYSCALL_STATUS_OK) cleanup_ok = false;
		loaded.load_cap = CAP_ID_INVALID;
	}
	return status == SYSCALL_STATUS_OK && cleanup_ok;
}
