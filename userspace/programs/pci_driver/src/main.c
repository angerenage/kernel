#include <protocol/device.h>
#include <stddef.h>
#include <stdio.h>
#include <system/capability.h>

#include "pci.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	cap_id_t device_cap;
	cap_id_t memory_allocator_cap;
	cap_id_t interrupts_cap;
	cap_id_t io_ports_cap;
	bool     success;

	(void)argc;
	(void)argv;
	if (capc != DEVICE_DRIVER_CAPABILITY_COUNT || capv == NULL ||
	    capv[DEVICE_DRIVER_CAPABILITY_DEVICE] == CAP_ID_INVALID ||
	    capv[DEVICE_DRIVER_CAPABILITY_MEMORY_ALLOCATOR] == CAP_ID_INVALID ||
	    capv[DEVICE_DRIVER_CAPABILITY_INTERRUPTS] == CAP_ID_INVALID)
		return 1;
	device_cap           = capv[DEVICE_DRIVER_CAPABILITY_DEVICE];
	memory_allocator_cap = capv[DEVICE_DRIVER_CAPABILITY_MEMORY_ALLOCATOR];
	interrupts_cap       = capv[DEVICE_DRIVER_CAPABILITY_INTERRUPTS];
	io_ports_cap         = capv[DEVICE_DRIVER_CAPABILITY_IO_PORTS];
	success              = pci_driver_run(device_cap, memory_allocator_cap, interrupts_cap, io_ports_cap);
	if (cap_drop(device_cap) != SYSCALL_STATUS_OK) success = false;
	if (cap_drop(memory_allocator_cap) != SYSCALL_STATUS_OK) success = false;
	if (cap_drop(interrupts_cap) != SYSCALL_STATUS_OK) success = false;
	if (io_ports_cap != CAP_ID_INVALID && cap_drop(io_ports_cap) != SYSCALL_STATUS_OK) success = false;
	if (!success) {
		static const char message[] = "pci: driver stopped after an error\n";
		(void)fwrite(message, 1u, sizeof(message) - 1u, stderr);
	}
	return success ? 0 : 1;
}
