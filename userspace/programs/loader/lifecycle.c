#include "lifecycle.h"

#include <stdlib.h>
#include <system/capability.h>

static uint64_t loader_object_id(const struct loader_loaded_program* program) {
	return (uint64_t)(uintptr_t)program;
}

syscall_status_t loader_unpublish_terminal(channel_id_t endpoint, struct loader_loaded_program* program) {
	if (program == NULL || program->load_cap == CAP_ID_INVALID) return SYSCALL_STATUS_OK;
	syscall_status_t status = cap_unpublish(endpoint, loader_object_id(program));
	if (status == SYSCALL_STATUS_OK) program->load_cap = CAP_ID_INVALID;
	return status;
}

bool loader_unpublish_abandoned(channel_id_t endpoint, struct loader_loaded_program* program) {
	if (program == NULL || program->load_cap == CAP_ID_INVALID) return false;
	if (cap_unpublish_if_unused(endpoint, loader_object_id(program)) != SYSCALL_STATUS_OK) return false;
	program->load_cap = CAP_ID_INVALID;
	return true;
}

void loader_release_program(struct loader_loaded_program* program) {
	if (program == NULL) return;
	if (program->process_cap != CAP_ID_INVALID) (void)cap_drop(program->process_cap);
	free(program);
}
