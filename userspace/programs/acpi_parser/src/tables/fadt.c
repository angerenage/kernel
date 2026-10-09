#include "fadt.h"

#include <base/acpi.h>
#include <runtime/device.h>
#include <stddef.h>
#include <stdint.h>
#include <system/acpi.h>
#include <system/capability.h>

#if defined(__x86_64__)
#include <base/io_port.h>
#include <system/io_port.h>

#include "../device.h"

/* FADT IA-PC Boot Architecture Flags: 8042 controller present (bit 1). */
#define FADT_IAPC_BOOT_ARCH_8042 (1u << 1u)
#define I8042_DATA_PORT 0x60u
#define I8042_COMMAND_PORT 0x64u

#define I8042_COMPATIBLE "acpi:i8042"
#define I8042_DATA_RESOURCE "data_port"
#define I8042_COMMAND_RESOURCE "status_command_port"

/* The builder delegates resources to the manager, so the temporary caps need CAP_DELEGATE. */
#define I8042_DRIVER_PORT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP))
#define I8042_PARSER_PORT_RIGHTS (I8042_DRIVER_PORT_RIGHTS | CAP_DELEGATE)

static syscall_status_t submit_i8042(cap_id_t root_cap, cap_id_t io_ports_cap) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	struct io_port_info   range;
	cap_id_t              data_cap    = CAP_ID_INVALID;
	cap_id_t              command_cap = CAP_ID_INVALID;
	syscall_status_t      status;
	syscall_status_t      drop_status;

	status = io_port_info(io_ports_cap, &range);
	if (status != SYSCALL_STATUS_OK) return status;
	if (range.base > I8042_DATA_PORT || range.count <= I8042_COMMAND_PORT - range.base)
		return SYSCALL_STATUS_UNAVAILABLE;

	status = io_port_derive(io_ports_cap, I8042_DATA_PORT - range.base, 1u, I8042_PARSER_PORT_RIGHTS, &data_cap);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	status = io_port_derive(io_ports_cap, I8042_COMMAND_PORT - range.base, 1u, I8042_PARSER_PORT_RIGHTS, &command_cap);
	if (status != SYSCALL_STATUS_OK) goto cleanup;

	status = device_builder_begin_root(root_cap, &builder);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_compatible(&builder, I8042_COMPATIBLE);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(
			&builder, I8042_DATA_RESOURCE, sizeof(I8042_DATA_RESOURCE) - 1u, data_cap, I8042_DRIVER_PORT_RIGHTS);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(&builder,
		                                     I8042_COMMAND_RESOURCE,
		                                     sizeof(I8042_COMMAND_RESOURCE) - 1u,
		                                     command_cap,
		                                     I8042_DRIVER_PORT_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);

cleanup:
	if (command_cap != CAP_ID_INVALID) {
		drop_status = cap_drop(command_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	if (data_cap != CAP_ID_INVALID) {
		drop_status = cap_drop(data_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	return status;
}
#endif

syscall_status_t acpi_fadt_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t io_ports_cap,
                                 size_t* device_count) {
#if defined(__x86_64__)
	struct acpi_provider_fadt_read_response fadt;
	syscall_status_t                        status;
#endif

	if (provider_cap == CAP_ID_INVALID || root_cap == CAP_ID_INVALID || device_count == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
#if defined(__x86_64__)
	status = acpi_fadt_read(provider_cap, &fadt);
	if (status == SYSCALL_STATUS_UNAVAILABLE) return SYSCALL_STATUS_OK;
	if (status != SYSCALL_STATUS_OK) return status;
	if ((fadt.iapc_boot_arch & FADT_IAPC_BOOT_ARCH_8042) == 0u) return SYSCALL_STATUS_OK;
	if (io_ports_cap == CAP_ID_INVALID) return SYSCALL_STATUS_UNAVAILABLE;
	if (*device_count == SIZE_MAX) return SYSCALL_STATUS_FAILED;
	status = submit_i8042(root_cap, io_ports_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	(*device_count)++;
#else
	(void)io_ports_cap;
#endif
	return SYSCALL_STATUS_OK;
}
