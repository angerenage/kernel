#include <base/loader.h>
#include <base/module.h>
#include <base/startup.h>
#include <runtime/init.h>
#include <stdbool.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/loader.h>
#include <system/module.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>
#include <system/time.h>

extern struct init_startup_info g_startup;

static channel_id_t init_endpoint = CHANNEL_ID_INVALID;

static bool init_signal_wait_message_valid(const struct signal_message* message, process_id_t sender) {
	if (message == NULL || message->sender != sender) return false;
	if (message->payload.args[0] != INIT_SIGNAL_WAIT_COOKIE) return false;
	if (message->payload.args[1] != 11u) return false;
	if (message->payload.args[2] != 22u) return false;
	return message->payload.args[3] == 33u;
}

static syscall_status_t publish_init_capability(process_id_t target, cap_id_t* out_cap) {
	syscall_status_t status;

	if (init_endpoint == CHANNEL_ID_INVALID) {
		status = channel_create(&init_endpoint);
		if (status != SYSCALL_STATUS_OK) return status;
	}

	return cap_publish(init_endpoint, INIT_SERVICE_OBJECT_ID, target, CAP_CALL | CAP_MANAGE | CAP_DELEGATE, out_cap);
}

static bool reply_request(cap_call_id_t call_id, const void* response, size_t response_size,
                          syscall_status_t response_status) {
	syscall_status_t status = channel_reply(call_id, response, response_size, response_status);

	if (status == SYSCALL_STATUS_OK) return true;
	printf("init: channel reply failed: %u\n", (unsigned)status);
	return false;
}

static int handle_requests(process_id_t loader_pid, process_id_t init_pid, cap_id_t signal_cap) {
	union {
		struct init_request_header      header;
		struct init_ping_request        ping;
		struct init_get_signal_request  get_signal;
		struct init_signal_back_request signal_back;
		struct init_stop_request        stop;
	} buffer;
	struct cap_request request;
	bool               received;
	bool               signal_cap_delegated = false;
	bool               signal_wait_received = false;
	bool               signal_back_sent     = false;
	syscall_status_t   status;

	for (;;) {
		received = false;
		status   = channel_recv(init_endpoint, &request, &buffer, sizeof(buffer), &received);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: channel receive failed: %u\n", (unsigned)status);
			return 1;
		}
		if (!received) {
			(void)sched_yield();
			continue;
		}

		if (request.object_id != INIT_SERVICE_OBJECT_ID || (request.rights & CAP_CALL) == 0u ||
		    request.request_size < sizeof(buffer.header)) {
			if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
			continue;
		}
		if (request.caller != loader_pid) {
			if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_DENIED)) return 1;
			continue;
		}

		switch (buffer.header.op) {
		case INIT_OP_PING: {
			if (!signal_wait_received || request.request_size != sizeof(buffer.ping) ||
			    request.response_capacity < sizeof(struct init_ping_response)) {
				if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
				continue;
			}

			printf("init: received ping value=%llu from pid=%llu\n",
			       (unsigned long long)buffer.ping.value,
			       (unsigned long long)request.caller);
			const struct init_ping_response response = {.value = buffer.ping.value + 1u};
			if (!reply_request(request.call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return 1;
			break;
		}
		case INIT_OP_GET_SIGNAL: {
			struct init_get_signal_response response;
			struct signal_message           message;
			cap_id_t                        delegated_cap;

			if (signal_cap_delegated || request.request_size != sizeof(buffer.get_signal) ||
			    request.response_capacity < sizeof(response)) {
				if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
				continue;
			}
			status = cap_delegate(signal_cap, request.caller, CAP_SIGNAL, &delegated_cap);
			if (status != SYSCALL_STATUS_OK) {
				if (!reply_request(request.call_id, NULL, 0u, status)) return 1;
				return 1;
			}
			response = (struct init_get_signal_response){
				.signal_cap = delegated_cap,
				.init_pid   = init_pid,
			};
			if (!reply_request(request.call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return 1;
			signal_cap_delegated = true;

			status = signal_wait(signal_cap, &message);
			if (status != SYSCALL_STATUS_OK || !init_signal_wait_message_valid(&message, loader_pid)) {
				printf("init: loader Signal wait failed or returned invalid payload: %u\n", (unsigned)status);
				return 1;
			}
			printf("init: received Signal from loader pid=%llu\n", (unsigned long long)message.sender);
			status = signal_unsubscribe(signal_cap);
			if (status != SYSCALL_STATUS_OK) {
				printf("init: Signal wait unsubscribe failed: %u\n", (unsigned)status);
				return 1;
			}
			signal_wait_received = true;
			break;
		}
		case INIT_OP_SIGNAL_BACK: {
			struct signal_send_response response;

			if (!signal_wait_received || signal_back_sent || request.request_size != sizeof(buffer.signal_back) ||
			    buffer.signal_back.signal_cap == CAP_ID_INVALID) {
				if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
				continue;
			}
			status = signal_send(buffer.signal_back.signal_cap,
			                     INIT_SIGNAL_UPCALL_COOKIE,
			                     44u,
			                     55u,
			                     66u,
			                     SIGNAL_SEND_FLAG_NONE,
			                     &response);
			if (status != SYSCALL_STATUS_OK) {
				printf("init: reverse Signal publication failed: %u\n", (unsigned)status);
				if (!reply_request(request.call_id, NULL, 0u, status)) return 1;
				return 1;
			}
			if (response.receiver_count != 1u || response.delivery_count != 1u) {
				printf("init: reverse Signal delivery mismatch receivers=%llu deliveries=%llu\n",
				       (unsigned long long)response.receiver_count,
				       (unsigned long long)response.delivery_count);
				if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_FAILED)) return 1;
				return 1;
			}
			signal_back_sent = true;
			if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_OK)) return 1;
			break;
		}
		case INIT_OP_STOP:
			if (!signal_back_sent || request.request_size != sizeof(buffer.stop)) {
				if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
				continue;
			}
			if ((request.rights & CAP_MANAGE) == 0u) {
				if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_DENIED)) return 1;
				continue;
			}
			printf("init: stop requested by pid=%llu\n", (unsigned long long)request.caller);
			if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_OK)) return 1;
			return 0;
		default:
			if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
			break;
		}
	}
}

static int launch_loader(void) {
	struct module_query_response module;
	struct loader_load_response  loaded;
	struct process_info_response process_info;
	struct process_startup_info  startup;
	struct self_info             self_info;
	cap_id_t                     init_cap;
	cap_id_t                     serial_cap;
	cap_id_t                     signal_cap;
	cap_id_t                     thread_cap;
	uintptr_t                    exit_code;
	syscall_status_t             status;

	status = module_resolve("loader.elf", sizeof("loader.elf"), &module);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf module resolution failed: %u\n", (unsigned)status);
		return 1;
	}

	status = loader_load(g_startup.loader_cap, module.cap, &loaded);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf load failed: %u\n", (unsigned)status);
		return 1;
	}

	status = process_get_info(loaded.process_cap, &process_info);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loaded process query failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	status = publish_init_capability(process_info.pid, &init_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: init capability publication failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	status = cap_delegate(g_startup.base.serial_cap, process_info.pid, CAP_WRITE | CAP_CALL, &serial_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: serial capability delegation failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}
	status = process_self_info(&self_info);
	if (status != SYSCALL_STATUS_OK || self_info.pid == PROCESS_PID_INVALID) {
		printf("init: self process query failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}
	status = signal_create(&signal_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: Signal creation failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	startup = (struct process_startup_info){
		.size            = sizeof(startup),
		.heap_base       = loaded.heap_base,
		.heap_page_count = loaded.heap_page_count,
		.page_size       = g_startup.base.page_size,
		.serial_cap      = serial_cap,
		.init_cap        = init_cap,
	};
	status = process_run(loaded.process_cap, loaded.entry, &startup, sizeof(startup), &thread_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process start failed: %u\n", (unsigned)status);
		(void)signal_destroy(signal_cap);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	printf("init: launched loader.elf pid=%llu process_cap=%llu thread_cap=%llu\n",
	       (unsigned long long)process_info.pid,
	       (unsigned long long)loaded.process_cap,
	       (unsigned long long)thread_cap);

	if (handle_requests(process_info.pid, self_info.pid, signal_cap) != 0) {
		(void)signal_destroy(signal_cap);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	status = thread_join(thread_cap, &exit_code);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader main thread wait failed: %u\n", (unsigned)status);
		(void)signal_destroy(signal_cap);
		return 1;
	}
	status = process_wait(loaded.process_cap, NULL);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process reap failed: %u\n", (unsigned)status);
		(void)signal_destroy(signal_cap);
		return 1;
	}
	printf("init: loader.elf exited with code %llu\n", (unsigned long long)exit_code);
	status = signal_destroy(signal_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: Signal destroy failed: %u\n", (unsigned)status);
		return 1;
	}
	return 0;
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	if (g_startup.loader_cap == CAP_ID_INVALID) {
		printf("init: loader capability not available\n");
		return 1;
	}

	if (g_startup.base.serial_cap == CAP_ID_INVALID) {
		printf("init: serial capability not available\n");
		return 1;
	}

	if (launch_loader() != 0) {
		printf("init: loader launch failed\n");
		return 1;
	}

	return 0;
}
