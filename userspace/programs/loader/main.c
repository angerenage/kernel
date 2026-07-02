#include <runtime/init.h>
#include <stdio.h>
#include <system/capability.h>

int main(int argc, char** argv) {
	const struct init_ping_request ping_request = {
		.header = {.op = INIT_OP_PING},
		.value  = 41u,
	};
	const struct init_stop_request stop_request = {
		.header = {.op = INIT_OP_STOP},
	};
	struct init_ping_response ping_response;
	size_t                    response_size;
	syscall_status_t          status;

	(void)argc;
	(void)argv;

	printf("loader: ready to load\n");
	if (init_cap_id == CAP_ID_INVALID) {
		printf("loader: init capability not available\n");
		return 1;
	}

	status = cap_call(
		init_cap_id, &ping_request, sizeof(ping_request), &ping_response, sizeof(ping_response), &response_size);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: init ping failed: %u\n", (unsigned)status);
		return 1;
	}
	if (response_size != sizeof(ping_response) || ping_response.value != ping_request.value + 1u) {
		printf("loader: invalid init ping response\n");
		return 1;
	}
	printf("loader: init replied with value=%llu\n", (unsigned long long)ping_response.value);

	status = cap_call(init_cap_id, &stop_request, sizeof(stop_request), NULL, 0u, NULL);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: init stop request failed: %u\n", (unsigned)status);
		return 1;
	}
	printf("loader: init stop acknowledged\n");

	return 0;
}
