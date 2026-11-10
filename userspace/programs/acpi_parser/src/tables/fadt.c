#include "fadt.h"

#include <base/acpi.h>
#include <base/interrupt.h>
#include <system/acpi.h>

#if defined(__x86_64__)

#include "../device.h"

/* FADT IA-PC Boot Architecture Flags: 8042 controller present (bit 1). */
#define FADT_IAPC_BOOT_ARCH_8042 (1u << 1u)
#define I8042_DATA_PORT 0x60u
#define I8042_COMMAND_PORT 0x64u

#define I8042_COMPATIBLE "acpi:i8042"
#define I8042_AML_KEYBOARD_COMPATIBLE "acpi:PNP0303"
#define I8042_KEYBOARD_INTERRUPT_RESOURCE "keyboard_interrupt"
#define I8042_MOUSE_INTERRUPT_RESOURCE "mouse_interrupt"
#define I8042_DATA_RESOURCE "data_port"
#define I8042_COMMAND_RESOURCE "status_command_port"
#define I8042_KEYBOARD_INTERRUPT 1u
#define I8042_MOUSE_INTERRUPT 12u

/* The builder delegates resources to the manager, so the temporary caps need CAP_DELEGATE. */
#define I8042_DRIVER_PORT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP))
#define I8042_PARSER_PORT_RIGHTS (I8042_DRIVER_PORT_RIGHTS | CAP_DELEGATE)

static syscall_status_t stage_i8042(struct acpi_device_set* devices) {
	struct acpi_device* device;
	syscall_status_t    status;
	status = acpi_device_begin(devices, &device);
	if (status != SYSCALL_STATUS_OK) return status;
	status = acpi_device_add_compatible(device, I8042_COMPATIBLE);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_stage_interrupt(device,
		                                     I8042_KEYBOARD_INTERRUPT_RESOURCE,
		                                     I8042_KEYBOARD_INTERRUPT,
		                                     INTERRUPT_TRIGGER_FIRMWARE,
		                                     INTERRUPT_POLARITY_FIRMWARE);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_stage_interrupt(device,
		                                     I8042_MOUSE_INTERRUPT_RESOURCE,
		                                     I8042_MOUSE_INTERRUPT,
		                                     INTERRUPT_TRIGGER_FIRMWARE,
		                                     INTERRUPT_POLARITY_FIRMWARE);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_stage_io(device, I8042_DATA_RESOURCE, I8042_DATA_PORT, 1u);
	if (status == SYSCALL_STATUS_OK)
		status = acpi_device_stage_io(device, I8042_COMMAND_RESOURCE, I8042_COMMAND_PORT, 1u);
	return status;
}
#endif

syscall_status_t acpi_fadt_parse(cap_id_t provider_cap, struct acpi_device_set* devices) {
#if defined(__x86_64__)
	struct acpi_provider_fadt_read_response fadt;
	struct acpi_device*                     aml_keyboard;
	syscall_status_t                        status;
#endif

	if (provider_cap == CAP_ID_INVALID || devices == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
#if defined(__x86_64__)
	aml_keyboard = acpi_device_find_compatible(devices, I8042_AML_KEYBOARD_COMPATIBLE);
	if (aml_keyboard != NULL) return SYSCALL_STATUS_OK;
	status = acpi_fadt_read(provider_cap, &fadt);
	if (status == SYSCALL_STATUS_UNAVAILABLE) return SYSCALL_STATUS_OK;
	if (status != SYSCALL_STATUS_OK) return status;
	if ((fadt.iapc_boot_arch & FADT_IAPC_BOOT_ARCH_8042) == 0u) return SYSCALL_STATUS_OK;
	return stage_i8042(devices);
#endif
	return SYSCALL_STATUS_OK;
}
