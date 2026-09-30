#include "ps2.h"

#include <base/device.h>
#include <base/hardware/ps2.h>
#include <firmware/acpi.h>
#include <kernel/device.h>
#include <stdint.h>
#include <string.h>

#define ACPI_FADT_IAPC_BOOT_ARCH_OFFSET 109u
#define ACPI_FADT_IAPC_BOOT_ARCH_8042 (1u << 1u)
#define PS2_I8042_DATA_PORT 0x60u
#define PS2_I8042_STATUS_COMMAND_PORT 0x64u

static bool ps2_i8042_present(void) {
#if defined(PLATFORM_PC_X86_64)
	const struct acpi_sdt_header* fadt = acpi_table_next("FACP", NULL);
	uint16_t                      boot_arch;
	uint32_t                      length;

	if (fadt == NULL || fadt->revision < 2u) return false;
	memcpy(&length, &fadt->length, sizeof(length));
	if (length < ACPI_FADT_IAPC_BOOT_ARCH_OFFSET + sizeof(boot_arch)) return false;
	memcpy(&boot_arch, (const uint8_t*)fadt + ACPI_FADT_IAPC_BOOT_ARCH_OFFSET, sizeof(boot_arch));
	return (boot_arch & ACPI_FADT_IAPC_BOOT_ARCH_8042) != 0u;
#else
	return false;
#endif
}

bool kernel_device_register_ps2_controllers(void) {
	struct ps2_controller controller;

	if (!kernel_device_register_type(KERNEL_DEVICE_TYPE_PS2_CONTROLLER, sizeof(controller))) return false;
	if (!ps2_i8042_present()) return true;
	memset(&controller, 0, sizeof(controller));
	controller.interface                                = PS2_CONTROLLER_INTERFACE_I8042_IO_PORT;
	controller.access.i8042_io_port.data_port           = PS2_I8042_DATA_PORT;
	controller.access.i8042_io_port.status_command_port = PS2_I8042_STATUS_COMMAND_PORT;
	return kernel_device_register(KERNEL_DEVICE_TYPE_PS2_CONTROLLER, &controller, sizeof(controller));
}
