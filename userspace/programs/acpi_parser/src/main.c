#include <base/cap.h>
#include <protocol/device_manager.h>
#include <stddef.h>
#include <stdio.h>
#include <system/capability.h>

#include "parser.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	size_t           devices = 0u;
	syscall_status_t status;

	(void)argc;
	(void)argv;
	if (capc != DEVICE_PARSER_CAPABILITY_COUNT || capv == NULL ||
	    capv[DEVICE_PARSER_CAPABILITY_FIRMWARE] == CAP_ID_INVALID ||
	    capv[DEVICE_PARSER_CAPABILITY_DEVICE_ROOT] == CAP_ID_INVALID ||
	    capv[DEVICE_PARSER_CAPABILITY_MEMORY_ALLOCATOR] == CAP_ID_INVALID)
		return 1;
	status = acpi_parser_parse(capv[DEVICE_PARSER_CAPABILITY_FIRMWARE],
	                           capv[DEVICE_PARSER_CAPABILITY_DEVICE_ROOT],
	                           capv[DEVICE_PARSER_CAPABILITY_MEMORY_ALLOCATOR],
	                           capv[DEVICE_PARSER_CAPABILITY_IO_PORTS],
	                           &devices);
	if (cap_drop(capv[DEVICE_PARSER_CAPABILITY_FIRMWARE]) != SYSCALL_STATUS_OK && status == SYSCALL_STATUS_OK)
		status = SYSCALL_STATUS_FAILED;
	if (cap_drop(capv[DEVICE_PARSER_CAPABILITY_DEVICE_ROOT]) != SYSCALL_STATUS_OK && status == SYSCALL_STATUS_OK)
		status = SYSCALL_STATUS_FAILED;
	if (cap_drop(capv[DEVICE_PARSER_CAPABILITY_MEMORY_ALLOCATOR]) != SYSCALL_STATUS_OK && status == SYSCALL_STATUS_OK)
		status = SYSCALL_STATUS_FAILED;
	if (capv[DEVICE_PARSER_CAPABILITY_IO_PORTS] != CAP_ID_INVALID &&
	    cap_drop(capv[DEVICE_PARSER_CAPABILITY_IO_PORTS]) != SYSCALL_STATUS_OK && status == SYSCALL_STATUS_OK)
		status = SYSCALL_STATUS_FAILED;
	if (status != SYSCALL_STATUS_OK) {
		printf("acpi-parser: parsing failed: %u\n", (unsigned)status);
		return 1;
	}
	printf("acpi-parser: created %zu root device(s)\n", devices);
	return 0;
}
