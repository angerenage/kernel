#include <runtime/init.h>
#include <stdbool.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/signal.h>
#include <system/upcall.h>

static bool         loader_signal_upcall_received;
static process_id_t loader_signal_upcall_sender;
static uint64_t     loader_signal_upcall_args[4];

DEFINE_UPCALL_HANDLER(loader_signal_handler, arg0, arg1, arg2, arg3, arg4) {
	__atomic_store_n(&loader_signal_upcall_sender, (process_id_t)arg0, __ATOMIC_RELAXED);
	__atomic_store_n(&loader_signal_upcall_args[0], (uint64_t)arg1, __ATOMIC_RELAXED);
	__atomic_store_n(&loader_signal_upcall_args[1], (uint64_t)arg2, __ATOMIC_RELAXED);
	__atomic_store_n(&loader_signal_upcall_args[2], (uint64_t)arg3, __ATOMIC_RELAXED);
	__atomic_store_n(&loader_signal_upcall_args[3], (uint64_t)arg4, __ATOMIC_RELAXED);
	__atomic_store_n(&loader_signal_upcall_received, true, __ATOMIC_RELEASE);
}

static bool loader_signal_upcall_valid(process_id_t sender) {
	if (!__atomic_load_n(&loader_signal_upcall_received, __ATOMIC_ACQUIRE)) return false;
	if (__atomic_load_n(&loader_signal_upcall_sender, __ATOMIC_RELAXED) != sender) return false;
	if (__atomic_load_n(&loader_signal_upcall_args[0], __ATOMIC_RELAXED) != INIT_SIGNAL_UPCALL_COOKIE) return false;
	if (__atomic_load_n(&loader_signal_upcall_args[1], __ATOMIC_RELAXED) != 44u) return false;
	if (__atomic_load_n(&loader_signal_upcall_args[2], __ATOMIC_RELAXED) != 55u) return false;
	return __atomic_load_n(&loader_signal_upcall_args[3], __ATOMIC_RELAXED) == 66u;
}

int main(int argc, char** argv) {
	const struct init_get_signal_request get_signal_request = {
		.header = {.op = INIT_OP_GET_SIGNAL},
	};
	const struct init_ping_request ping_request = {
		.header = {.op = INIT_OP_PING},
		.value  = 41u,
	};
	const struct init_stop_request stop_request = {
		.header = {.op = INIT_OP_STOP},
	};
	struct init_get_signal_response get_signal_response;
	struct init_ping_response       ping_response;
	struct init_signal_back_request signal_back_request;
	struct signal_message           signal_message;
	struct signal_read_response     signal_read_response;
	struct signal_send_response     signal_response;
	cap_id_t                        init_signal_cap;
	cap_id_t                        loader_signal_cap;
	bool                            signal_has_value;
	bool                            signal_received;
	size_t                          response_size;
	syscall_status_t                status;

	(void)argc;
	(void)argv;

	printf("loader: ready to load\n");
	if (init_cap_id == CAP_ID_INVALID) {
		printf("loader: init capability not available\n");
		return 1;
	}

	response_size = 0u;
	status        = cap_call(init_cap_id,
                      &get_signal_request,
                      sizeof(get_signal_request),
                      &get_signal_response,
                      sizeof(get_signal_response),
                      &response_size);
	if (status != SYSCALL_STATUS_OK || response_size != sizeof(get_signal_response) ||
	    get_signal_response.signal_cap == CAP_ID_INVALID || get_signal_response.init_pid == PROCESS_PID_INVALID) {
		printf("loader: init Signal capability request failed: %u\n", (unsigned)status);
		return 1;
	}

	status = signal_read(get_signal_response.signal_cap, &signal_message, &signal_has_value);
	if (status != SYSCALL_STATUS_DENIED) {
		printf("loader: CAP_SIGNAL-only capability unexpectedly allowed read: %u\n", (unsigned)status);
		return 1;
	}
	status = signal_try_wait(get_signal_response.signal_cap, &signal_message, &signal_received);
	if (status != SYSCALL_STATUS_DENIED) {
		printf("loader: CAP_SIGNAL-only capability unexpectedly allowed wait: %u\n", (unsigned)status);
		return 1;
	}

	status = signal_send(get_signal_response.signal_cap,
	                     INIT_SIGNAL_WAIT_COOKIE,
	                     11u,
	                     22u,
	                     33u,
	                     SIGNAL_SEND_FLAG_NONE,
	                     &signal_response);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: Signal publication to init failed: %u\n", (unsigned)status);
		return 1;
	}
	printf("loader: signaled init receivers=%llu deliveries=%llu\n",
	       (unsigned long long)signal_response.receiver_count,
	       (unsigned long long)signal_response.delivery_count);

	response_size = 0u;
	status        = cap_call(
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

	status = signal_create(&loader_signal_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: reverse Signal creation failed: %u\n", (unsigned)status);
		return 1;
	}
	__atomic_store_n(&loader_signal_upcall_received, false, __ATOMIC_RELEASE);
	status = signal_set_handler(loader_signal_cap, loader_signal_handler, SIGNAL_HANDLER_FLAG_ONESHOT);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: reverse Signal handler registration failed: %u\n", (unsigned)status);
		(void)signal_destroy(loader_signal_cap);
		return 1;
	}
	status = cap_delegate(loader_signal_cap, get_signal_response.init_pid, CAP_SIGNAL, &init_signal_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: reverse Signal delegation failed: %u\n", (unsigned)status);
		(void)signal_destroy(loader_signal_cap);
		return 1;
	}

	signal_back_request = (struct init_signal_back_request){
		.header     = {.op = INIT_OP_SIGNAL_BACK},
		.signal_cap = init_signal_cap,
	};
	status = cap_call(init_cap_id, &signal_back_request, sizeof(signal_back_request), NULL, 0u, NULL);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: reverse Signal request failed: %u\n", (unsigned)status);
		(void)signal_destroy(loader_signal_cap);
		return 1;
	}
	if (!loader_signal_upcall_valid(get_signal_response.init_pid)) {
		printf("loader: reverse Signal upcall was not delivered correctly\n");
		(void)signal_destroy(loader_signal_cap);
		return 1;
	}
	printf("loader: received Signal upcall from init pid=%llu\n", (unsigned long long)get_signal_response.init_pid);

	status = signal_read_info(loader_signal_cap, &signal_read_response);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: reverse Signal info read failed: %u\n", (unsigned)status);
		(void)signal_destroy(loader_signal_cap);
		return 1;
	}
	if (signal_read_response.generation != 1u || signal_read_response.handler_count != 0u ||
	    signal_read_response.wait_subscription_count != 0u || signal_read_response.blocked_waiter_count != 0u ||
	    signal_read_response.caller_upcall_pending_count != 0u ||
	    signal_read_response.caller_upcall_dropped_count != 0u || signal_read_response.caller_upcall_capacity == 0u ||
	    (signal_read_response.flags & SIGNAL_READ_FLAG_HAS_VALUE) == 0u) {
		printf("loader: invalid reverse Signal info snapshot\n");
		(void)signal_destroy(loader_signal_cap);
		return 1;
	}

	status = signal_destroy(loader_signal_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: reverse Signal destroy failed: %u\n", (unsigned)status);
		return 1;
	}

	status = cap_call(init_cap_id, &stop_request, sizeof(stop_request), NULL, 0u, NULL);
	if (status != SYSCALL_STATUS_OK) {
		printf("loader: init stop request failed: %u\n", (unsigned)status);
		return 1;
	}
	printf("loader: init stop acknowledged\n");

	return 0;
}
