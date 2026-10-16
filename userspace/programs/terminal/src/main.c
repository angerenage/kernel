#include <base/cap.h>
#include <base/framebuffer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/framebuffer.h>
#include <system/memory.h>

#include "server.h"
#include "terminal.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct framebuffer_info_response info;
	struct framebuffer_map_response  mapping  = {.mapping_cap = CAP_ID_INVALID};
	struct terminal*                 terminal = NULL;
	cap_id_t                         framebuffer_cap;
	syscall_status_t                 status;
	int                              result = 1;

	(void)argv;
	if (argc != 0 || capc != 1u || capv == NULL || capv[0] == CAP_ID_INVALID) {
		printf("terminal: invalid startup arguments\n");
		return 1;
	}
	framebuffer_cap = capv[0];
	status          = framebuffer_get_info(framebuffer_cap, &info);
	if (status != SYSCALL_STATUS_OK) {
		printf("terminal: framebuffer query failed: %u\n", (unsigned)status);
		goto cleanup;
	}
	status = framebuffer_map(framebuffer_cap, &mapping);
	if (status != SYSCALL_STATUS_OK) {
		printf("terminal: framebuffer mapping failed: %u\n", (unsigned)status);
		goto cleanup;
	}
	if (mapping.data_offset > mapping.mapping_size || info.size > mapping.mapping_size - mapping.data_offset) {
		printf("terminal: invalid framebuffer mapping\n");
		goto cleanup;
	}
	terminal = terminal_create((volatile uint8_t*)mapping.address + mapping.data_offset, &info);
	if (terminal == NULL) {
		printf("terminal: initialization failed\n");
		goto cleanup;
	}
	status = cap_drop(framebuffer_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("terminal: framebuffer capability drop failed: %u\n", (unsigned)status);
		goto cleanup;
	}
	framebuffer_cap = CAP_ID_INVALID;
	result          = terminal_server_run(terminal);

cleanup:
	terminal_destroy(terminal);
	if (mapping.mapping_cap != CAP_ID_INVALID && mapping_unmap(mapping.mapping_cap) != SYSCALL_STATUS_OK) result = 1;
	if (framebuffer_cap != CAP_ID_INVALID && cap_drop(framebuffer_cap) != SYSCALL_STATUS_OK) result = 1;
	return result;
}
