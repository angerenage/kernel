#include "supervisor.h"

#include <base/startup.h>
#include <protocol/device.h>
#include <protocol/vfs.h>
#include <runtime/diagnostic.h>
#include <runtime/program.h>
#include <runtime/registry.h>
#include <runtime/stream.h>
#include <runtime/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/thread.h>
#include <system/time.h>

#include "server.h"

#define DM_DRIVER_LOADER_SERVICE "elf64"
#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 0u
#define DM_DRIVER_LOG_CAPACITY (16u * 1024u)
#define DM_DRIVER_RESTART_LIMIT 5u
#define DM_DRIVER_RESTART_BASE_MS 250u
#define DM_DRIVER_RESTART_MAX_MS 4000u

#define DM_SUPERVISOR_CONTROL_OBJECT_ID (DEVICE_INTERNAL_OBJECT_BASE + 0u)
#define DM_SUPERVISOR_STDOUT_OBJECT_ID (DEVICE_INTERNAL_OBJECT_BASE + 1u)
#define DM_SUPERVISOR_STDERR_OBJECT_ID (DEVICE_INTERNAL_OBJECT_BASE + 2u)

enum dm_supervisor_op {
	DM_SUPERVISOR_OP_PROCESS_EXIT = 0u,
	DM_SUPERVISOR_OP_RETRY_READY,
};

struct dm_supervisor_request {
	enum dm_supervisor_op op;
	uint32_t              reserved;
	uint64_t              device_id;
	uint64_t              generation;
	uint64_t              wait_status;
	uint64_t              exit_code;
};

struct dm_monitor_argument {
	cap_id_t process_cap;
	cap_id_t control_cap;
	uint64_t device_id;
	uint64_t generation;
};

struct dm_retry_argument {
	cap_id_t control_cap;
	uint64_t device_id;
	uint64_t generation;
	size_t   delay_ms;
};

static bool reply(const struct cap_request* call, const void* response, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, response, size, status) == SYSCALL_STATUS_OK;
}

static void log_append(struct dm_driver_log* log, const void* data, size_t size) {
	const uint8_t* bytes = data;
	if (size >= DM_DRIVER_LOG_CAPACITY) {
		if (log->data == NULL) log->data = malloc(DM_DRIVER_LOG_CAPACITY);
		if (log->data == NULL) return;
		memcpy(log->data, bytes + size - DM_DRIVER_LOG_CAPACITY, DM_DRIVER_LOG_CAPACITY);
		log->size = DM_DRIVER_LOG_CAPACITY;
		return;
	}
	if (log->data == NULL) {
		log->data = malloc(DM_DRIVER_LOG_CAPACITY);
		if (log->data == NULL) return;
	}
	if (size > DM_DRIVER_LOG_CAPACITY - log->size) {
		size_t discard = size - (DM_DRIVER_LOG_CAPACITY - log->size);
		memmove(log->data, log->data + discard, log->size - discard);
		log->size -= discard;
	}
	memcpy(log->data + log->size, bytes, size);
	log->size += size;
}

static struct dm_driver_binding* find_binding_by_device(struct device_server* server, uint64_t device_id) {
	for (struct dm_driver_binding* binding = server->supervisor.bindings; binding != NULL; binding = binding->next)
		if (binding->device->id == device_id) return binding;
	return NULL;
}

static struct dm_driver_binding* find_binding_by_process(struct device_server* server, process_id_t process_id) {
	for (struct dm_driver_binding* binding = server->supervisor.bindings; binding != NULL; binding = binding->next)
		if (binding->process_id == process_id && process_id != PROCESS_PID_INVALID) return binding;
	return NULL;
}

static bool driver_rejected(const struct dm_driver_binding* binding, const struct dm_driver* driver) {
	if (binding == NULL) return false;
	for (size_t index = 0u; index < binding->rejected_count; index++)
		if (binding->rejected_drivers[index] == driver) return true;
	return false;
}

static struct dm_driver* match_driver(struct device_server* server, const struct dm_device* device,
                                      const struct dm_driver_binding* binding) {
	for (size_t index = 0u; index < device->compatible_count; index++) {
		const struct dm_string* compatible = &device->compatible_ids[index];
		for (struct dm_driver* driver = server->drivers.drivers; driver != NULL; driver = driver->next) {
			if (driver_rejected(binding, driver)) continue;
			for (size_t claimed = 0u; claimed < driver->compatible_count; claimed++) {
				if (compatible->size == driver->compatible_ids[claimed].size &&
				    memcmp(compatible->data, driver->compatible_ids[claimed].data, compatible->size) == 0)
					return driver;
			}
		}
	}
	return NULL;
}

static bool binding_reject_driver(struct dm_driver_binding* binding) {
	if (driver_rejected(binding, binding->driver)) return true;
	if (binding->rejected_count == SIZE_MAX ||
	    binding->rejected_count + 1u > SIZE_MAX / sizeof(*binding->rejected_drivers))
		return false;
	struct dm_driver** rejected =
		realloc(binding->rejected_drivers, (binding->rejected_count + 1u) * sizeof(*rejected));
	if (rejected == NULL) return false;
	binding->rejected_drivers                            = rejected;
	binding->rejected_drivers[binding->rejected_count++] = binding->driver;
	return true;
}

static syscall_status_t open_executable(struct dm_driver* driver) {
	static const struct registry_protocol_query vfs_query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle vfs        = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle  executable = {.capability = CAP_ID_INVALID};
	struct registry_call_result    acquire;
	struct filesystem_call_result  opened;
	syscall_status_t               status;

	if (driver->executable_cap != CAP_ID_INVALID) return SYSCALL_STATUS_OK;
	acquire = registry_acquire(&vfs_query, VFS_SERVICE_NAME, &vfs);
	if (acquire.transport_status != SYSCALL_STATUS_OK) return acquire.transport_status;
	if (acquire.status != REGISTRY_STATUS_OK) return SYSCALL_STATUS_UNAVAILABLE;
	opened = vfs_open(
		vfs.capability, driver->executable.data, driver->executable.size, CAP_READ | CAP_DELEGATE, &executable);
	status = cap_drop(vfs.capability);
	if (opened.transport_status != SYSCALL_STATUS_OK) {
		if (executable.capability != CAP_ID_INVALID) (void)cap_drop(executable.capability);
		return opened.transport_status;
	}
	if (opened.status != FILESYSTEM_STATUS_OK || executable.info.type != FILESYSTEM_NODE_FILE) {
		if (executable.capability != CAP_ID_INVALID) (void)cap_drop(executable.capability);
		return SYSCALL_STATUS_UNAVAILABLE;
	}
	if (status != SYSCALL_STATUS_OK) {
		(void)cap_drop(executable.capability);
		return status;
	}
	driver->executable_cap = executable.capability;
	return SYSCALL_STATUS_OK;
}

static void monitor_process(void* opaque) {
	const struct dm_monitor_argument* argument = opaque;
	struct dm_supervisor_request      request  = {
		.op         = DM_SUPERVISOR_OP_PROCESS_EXIT,
		.device_id  = argument->device_id,
		.generation = argument->generation,
	};
	uintptr_t exit_code = 0u;
	request.wait_status = process_wait(argument->process_cap, &exit_code);
	request.exit_code   = exit_code;
	thread_exit(cap_call(argument->control_cap, &request, sizeof(request), NULL, 0u, NULL) == SYSCALL_STATUS_OK ? 0u
	                                                                                                            : 1u);
}

static void retry_process(void* opaque) {
	const struct dm_retry_argument*    argument = opaque;
	const struct dm_supervisor_request request  = {
		.op         = DM_SUPERVISOR_OP_RETRY_READY,
		.device_id  = argument->device_id,
		.generation = argument->generation,
	};
	(void)sleep_ms(argument->delay_ms);
	thread_exit(cap_call(argument->control_cap, &request, sizeof(request), NULL, 0u, NULL) == SYSCALL_STATUS_OK ? 0u
	                                                                                                            : 1u);
}

static bool spawn_worker(struct device_server* server, uintptr_t entry, const void* argument, size_t argument_size,
                         const char* name) {
	cap_id_t thread_cap = CAP_ID_INVALID;
	/* Keep names local: an executable mapping is not necessarily readable as syscall string data. */
	syscall_status_t status =
		process_spawn_thread(server->supervisor.self_cap, entry, argument, argument_size, NULL, 0u, &thread_cap);
	if (status != SYSCALL_STATUS_OK) {
		char* diagnostic = runtime_diagnostic_get();
		printf("device-manager: worker %s spawn failed: %u%s%s\n",
		       name,
		       (unsigned)status,
		       diagnostic == NULL ? "" : ": ",
		       diagnostic == NULL ? "" : diagnostic);
		free(diagnostic);
	}
	if (status == SYSCALL_STATUS_OK) {
		status = thread_detach(thread_cap);
		if (status != SYSCALL_STATUS_OK)
			printf("device-manager: worker %s detach failed: %u\n", name, (unsigned)status);
	}
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (drop_status != SYSCALL_STATUS_OK)
			printf("device-manager: worker %s capability drop failed: %u\n", name, (unsigned)drop_status);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	return status == SYSCALL_STATUS_OK;
}

static void binding_schedule_retry(struct device_server* server, struct dm_driver_binding* binding) {
	if (binding->restart_count >= DM_DRIVER_RESTART_LIMIT) {
		binding->state = DM_DRIVER_QUARANTINED;
		printf("device-manager: quarantined %.*s for device %llu after %u failures\n",
		       (int)binding->driver->name.size,
		       binding->driver->name.data,
		       (unsigned long long)binding->device->id,
		       binding->restart_count);
		return;
	}
	size_t delay = DM_DRIVER_RESTART_BASE_MS << binding->restart_count;
	if (delay > DM_DRIVER_RESTART_MAX_MS) delay = DM_DRIVER_RESTART_MAX_MS;
	binding->restart_count++;
	binding->state                          = DM_DRIVER_BACKOFF;
	const struct dm_retry_argument argument = {
		.control_cap = server->supervisor.control_cap,
		.device_id   = binding->device->id,
		.generation  = binding->generation,
		.delay_ms    = delay,
	};
	if (!spawn_worker(server, (uintptr_t)retry_process, &argument, sizeof(argument), "dm/driver-retry")) {
		binding->state = DM_DRIVER_QUARANTINED;
		printf("device-manager: unable to schedule restart for %.*s\n",
		       (int)binding->driver->name.size,
		       binding->driver->name.data);
	}
}

static void binding_launch(struct device_server* server, struct dm_driver_binding* binding) {
	struct program_load_result loaded  = {.load_cap = CAP_ID_INVALID, .process_id = PROCESS_PID_INVALID};
	struct program_run_result  running = {.process_cap = CAP_ID_INVALID, .thread_cap = CAP_ID_INVALID};
	syscall_status_t           status;

	binding->generation++;
	binding->state       = DM_DRIVER_STARTING;
	binding->process_id  = PROCESS_PID_INVALID;
	binding->process_cap = CAP_ID_INVALID;
	status               = open_executable(binding->driver);
	if (status == SYSCALL_STATUS_UNAVAILABLE) {
		binding->last_wait_status = status;
		binding->state            = DM_DRIVER_QUARANTINED;
		printf("device-manager: driver executable unavailable for %.*s on device %llu\n",
		       (int)binding->driver->name.size,
		       binding->driver->name.data,
		       (unsigned long long)binding->device->id);
		return;
	}
	if (status != SYSCALL_STATUS_OK) goto failed;
	status = program_load(DM_DRIVER_LOADER_SERVICE,
	                      binding->driver->executable_cap,
	                      binding->driver->name.data,
	                      binding->driver->name.size,
	                      &loaded);
	if (status != SYSCALL_STATUS_OK) goto failed;
	binding->process_id = loaded.process_id;
	{
		struct program_capability_argument capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_DRIVER_CAPABILITY_COUNT] = {0};
		capv[PROCESS_STARTUP_CAP_STDIN].capability = CAP_ID_INVALID;
		capv[PROCESS_STARTUP_CAP_STDOUT]           = (struct program_capability_argument){
			.capability = server->supervisor.stdout_cap,
			.rights     = CAP_CALL | CAP_WRITE,
		};
		capv[PROCESS_STARTUP_CAP_STDERR] = (struct program_capability_argument){
			.capability = server->supervisor.stderr_cap,
			.rights     = CAP_CALL | CAP_WRITE,
		};
		capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_DRIVER_CAPABILITY_DEVICE] = (struct program_capability_argument){
			.capability = binding->device->manager_cap,
			.rights     = CAP_CALL | CAP_READ | CAP_MANAGE,
		};
		if ((binding->driver->root_capabilities & DM_DRIVER_CAPABILITY_MEMORY_ALLOCATOR) != 0u)
			capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_DRIVER_CAPABILITY_MEMORY_ALLOCATOR] =
				(struct program_capability_argument){
					.capability = server->memory_allocator_cap,
					.rights     = DEVICE_DRIVER_MEMORY_ALLOCATOR_CAP_RIGHTS,
			};
		if ((binding->driver->root_capabilities & DM_DRIVER_CAPABILITY_DMA) != 0u)
			capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_DRIVER_CAPABILITY_DMA] = (struct program_capability_argument){
				.capability = server->dma_cap,
				.rights     = server->dma_cap == CAP_ID_INVALID ? 0u : DEVICE_DRIVER_DMA_CAP_RIGHTS,
			};
		if ((binding->driver->root_capabilities & DM_DRIVER_CAPABILITY_INTERRUPTS) != 0u)
			capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_DRIVER_CAPABILITY_INTERRUPTS] =
				(struct program_capability_argument){
					.capability = server->interrupts_cap,
					.rights     = DEVICE_DRIVER_INTERRUPTS_CAP_RIGHTS,
			};
		if ((binding->driver->root_capabilities & DM_DRIVER_CAPABILITY_IO_PORTS) != 0u)
			capv[PROCESS_STARTUP_CAP_COUNT + DEVICE_DRIVER_CAPABILITY_IO_PORTS] = (struct program_capability_argument){
				.capability = server->io_ports_cap,
				.rights     = server->io_ports_cap == CAP_ID_INVALID ? 0u : DEVICE_DRIVER_IO_PORTS_CAP_RIGHTS,
			};
		status =
			program_run(&loaded, 0u, NULL, PROCESS_STARTUP_CAP_COUNT + DEVICE_DRIVER_CAPABILITY_COUNT, capv, &running);
	}
	if (status != SYSCALL_STATUS_OK) goto failed;
	loaded.load_cap      = CAP_ID_INVALID;
	binding->process_cap = running.process_cap;
	if (cap_drop(running.thread_cap) != SYSCALL_STATUS_OK) {
		status = SYSCALL_STATUS_FAILED;
		goto running_failed;
	}
	running.thread_cap                       = CAP_ID_INVALID;
	const struct dm_monitor_argument monitor = {
		.process_cap = binding->process_cap,
		.control_cap = server->supervisor.control_cap,
		.device_id   = binding->device->id,
		.generation  = binding->generation,
	};
	if (!spawn_worker(server, (uintptr_t)monitor_process, &monitor, sizeof(monitor), "dm/driver-monitor")) {
		status = SYSCALL_STATUS_FAILED;
		goto running_failed;
	}
	binding->state = DM_DRIVER_RUNNING;
	printf("device-manager: started %.*s for device %llu (generation %llu)\n",
	       (int)binding->driver->name.size,
	       binding->driver->name.data,
	       (unsigned long long)binding->device->id,
	       (unsigned long long)binding->generation);
	return;

running_failed:
	(void)process_kill(binding->process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	(void)process_wait(binding->process_cap, NULL);
	binding->process_cap = CAP_ID_INVALID;
failed:
	if (running.thread_cap != CAP_ID_INVALID) (void)cap_drop(running.thread_cap);
	if (loaded.load_cap != CAP_ID_INVALID) (void)program_cancel(loaded.load_cap);
	binding->process_id       = PROCESS_PID_INVALID;
	binding->last_wait_status = status;
	printf("device-manager: failed to start %.*s for device %llu: %u\n",
	       (int)binding->driver->name.size,
	       binding->driver->name.data,
	       (unsigned long long)binding->device->id,
	       (unsigned)status);
	binding_schedule_retry(server, binding);
}

static void binding_try_fallback(struct device_server* server, struct dm_driver_binding* binding) {
	if (!binding_reject_driver(binding)) {
		binding->state = DM_DRIVER_QUARANTINED;
		printf("device-manager: unable to record rejected driver for device %llu\n",
		       (unsigned long long)binding->device->id);
		return;
	}
	struct dm_driver* next = match_driver(server, binding->device, binding);
	if (next == NULL) {
		binding->state = DM_DRIVER_UNSUPPORTED;
		printf("device-manager: no remaining driver supports device %llu\n", (unsigned long long)binding->device->id);
		return;
	}
	printf("device-manager: trying %.*s for device %llu after %.*s declined it\n",
	       (int)next->name.size,
	       next->name.data,
	       (unsigned long long)binding->device->id,
	       (int)binding->driver->name.size,
	       binding->driver->name.data);
	binding->driver        = next;
	binding->restart_count = 0u;
	binding->state         = DM_DRIVER_UNBOUND;
	free(binding->stdout_log.data);
	free(binding->stderr_log.data);
	binding->stdout_log = (struct dm_driver_log){0};
	binding->stderr_log = (struct dm_driver_log){0};
	binding_launch(server, binding);
}

static bool in_generation_subtree(const struct dm_device* device, const struct dm_driver_binding* owner,
                                  uint64_t generation) {
	for (const struct dm_device* item = device; item != NULL; item = item->parent)
		if (item->provider_binding == owner && item->provider_generation == generation) return true;
	return false;
}

static bool subtree_has_active_binding(const struct device_server* server, const struct dm_driver_binding* owner,
                                       uint64_t generation) {
	for (const struct dm_driver_binding* binding = server->supervisor.bindings; binding != NULL;
	     binding                                 = binding->next) {
		if (!in_generation_subtree(binding->device, owner, generation)) continue;
		if (binding->state == DM_DRIVER_RUNNING || binding->state == DM_DRIVER_STARTING ||
		    binding->state == DM_DRIVER_STOPPING)
			return true;
	}
	return false;
}

static void abort_owned_builders(struct device_server* server, const struct dm_driver_binding* owner,
                                 uint64_t generation) {
	struct dm_builder* builder = server->state.builders;
	while (builder != NULL) {
		struct dm_builder* next = builder->next;
		if ((builder->owner_binding == owner && builder->owner_generation == generation) ||
		    (builder->parent != NULL && in_generation_subtree(builder->parent, owner, generation))) {
			(void)cap_unpublish(server->endpoint, builder->object_id);
			dm_builder_abort(&server->state, builder);
		}
		builder = next;
	}
}

static void stop_descendant_bindings(struct device_server* server, struct dm_driver_binding* owner,
                                     uint64_t generation) {
	for (struct dm_driver_binding* binding = server->supervisor.bindings; binding != NULL; binding = binding->next) {
		if (!in_generation_subtree(binding->device, owner, generation)) continue;
		if (binding->state == DM_DRIVER_RUNNING || binding->state == DM_DRIVER_STARTING) {
			binding->state = DM_DRIVER_STOPPING;
			if (binding->process_cap != CAP_ID_INVALID)
				(void)process_kill(binding->process_cap, PROCESS_EXIT_SYSTEM_UNKNOWN);
		}
		else {
			binding->generation++;
			binding->state      = DM_DRIVER_STOPPED;
			binding->process_id = PROCESS_PID_INVALID;
		}
	}
}

static void free_binding(struct dm_driver_binding* binding) {
	free(binding->stdout_log.data);
	free(binding->stderr_log.data);
	free(binding->rejected_drivers);
	free(binding);
}

static void remove_descendant_bindings(struct device_server* server, const struct dm_driver_binding* owner,
                                       uint64_t generation) {
	struct dm_driver_binding** cursor = &server->supervisor.bindings;
	while (*cursor != NULL) {
		struct dm_driver_binding* binding = *cursor;
		if (!in_generation_subtree(binding->device, owner, generation)) {
			cursor = &binding->next;
			continue;
		}
		binding->device->binding = NULL;
		*cursor                  = binding->next;
		free_binding(binding);
	}
}

static void remove_device_subtree(struct device_server* server, struct dm_device* device) {
	while (device->first_child != NULL) remove_device_subtree(server, device->first_child);
	(void)cap_unpublish(server->endpoint, device->id);
	dm_device_rollback(&server->state, device);
}

static void remove_generation_devices(struct device_server* server, const struct dm_driver_binding* owner,
                                      uint64_t generation) {
	for (;;) {
		struct dm_device* root = NULL;
		for (struct dm_device* device = server->state.devices; device != NULL; device = device->next) {
			if (device->provider_binding == owner && device->provider_generation == generation) {
				root = device;
				break;
			}
		}
		if (root == NULL) return;
		remove_device_subtree(server, root);
	}
}

static void finish_teardowns(struct device_server* server) {
	for (struct dm_driver_binding* owner = server->supervisor.bindings; owner != NULL; owner = owner->next) {
		if ((owner->state != DM_DRIVER_WAITING_FOR_CHILDREN && owner->state != DM_DRIVER_WAITING_FOR_FALLBACK) ||
		    subtree_has_active_binding(server, owner, owner->generation))
			continue;
		bool fallback = owner->state == DM_DRIVER_WAITING_FOR_FALLBACK;
		remove_descendant_bindings(server, owner, owner->generation);
		remove_generation_devices(server, owner, owner->generation);
		if (fallback) binding_try_fallback(server, owner);
		else binding_schedule_retry(server, owner);
	}
}

static void binding_exited(struct device_server* server, struct dm_driver_binding* binding,
                           syscall_status_t wait_status, uintptr_t exit_code) {
	bool stopping             = binding->state == DM_DRIVER_STOPPING;
	bool unsupported          = wait_status == SYSCALL_STATUS_OK && exit_code == DEVICE_DRIVER_EXIT_UNSUPPORTED;
	binding->process_cap      = CAP_ID_INVALID;
	binding->process_id       = PROCESS_PID_INVALID;
	binding->last_wait_status = wait_status;
	binding->last_exit_code   = exit_code;
	if (stopping) {
		binding->state = DM_DRIVER_STOPPED;
		finish_teardowns(server);
		return;
	}
	if (unsupported)
		printf("device-manager: %.*s declined device %llu\n",
		       (int)binding->driver->name.size,
		       binding->driver->name.data,
		       (unsigned long long)binding->device->id);
	else
		printf("device-manager: %.*s for device %llu exited: wait=%u code=%llu\n",
		       (int)binding->driver->name.size,
		       binding->driver->name.data,
		       (unsigned long long)binding->device->id,
		       (unsigned)wait_status,
		       (unsigned long long)exit_code);
	abort_owned_builders(server, binding, binding->generation);
	binding->state = unsupported ? DM_DRIVER_WAITING_FOR_FALLBACK : DM_DRIVER_WAITING_FOR_CHILDREN;
	stop_descendant_bindings(server, binding, binding->generation);
	finish_teardowns(server);
}

static bool dispatch_control(struct device_server* server, const struct cap_request* call, const void* data) {
	struct dm_supervisor_request request;
	if (call->caller != server->pid || call->request_size != sizeof(request) || data == NULL)
		return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	struct dm_driver_binding* binding = find_binding_by_device(server, request.device_id);
	if (binding == NULL || binding->generation != request.generation) return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
	if (request.op == DM_SUPERVISOR_OP_PROCESS_EXIT) {
		if (binding->state != DM_DRIVER_RUNNING && binding->state != DM_DRIVER_STOPPING)
			return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
		binding_exited(server, binding, (syscall_status_t)request.wait_status, (uintptr_t)request.exit_code);
	}
	else if (request.op == DM_SUPERVISOR_OP_RETRY_READY) {
		if (binding->state == DM_DRIVER_BACKOFF) binding_launch(server, binding);
	}
	else return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
}

static bool dispatch_output(struct device_server* server, const struct cap_request* call, const void* data,
                            bool error_stream) {
	struct stream_write_request  request;
	struct stream_write_response response;
	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (data == NULL || call->request_size < sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.header.op != STREAM_OP_WRITE || request.reserved != 0u ||
	    request.size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    call->request_size != sizeof(request) + (size_t)request.size)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	struct dm_driver_binding* binding = find_binding_by_process(server, call->caller);
	if (binding == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	const uint8_t* bytes = (const uint8_t*)data + sizeof(request);
	log_append(error_stream ? &binding->stderr_log : &binding->stdout_log, bytes, (size_t)request.size);
	if (request.size != 0u) {
		FILE* stream = error_stream ? stderr : stdout;
		printf("[%.*s:%llu:%s] ",
		       (int)binding->driver->name.size,
		       binding->driver->name.data,
		       (unsigned long long)binding->device->id,
		       error_stream ? "stderr" : "stdout");
		(void)fwrite(bytes, 1u, (size_t)request.size, stream);
		(void)fflush(stream);
	}
	response.size = request.size;
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

bool dm_supervisor_dispatch(struct device_server* server, const struct cap_request* call, const void* request) {
	if (call->object_id == DM_SUPERVISOR_CONTROL_OBJECT_ID) return dispatch_control(server, call, request);
	if (call->object_id == DM_SUPERVISOR_STDOUT_OBJECT_ID) return dispatch_output(server, call, request, false);
	if (call->object_id == DM_SUPERVISOR_STDERR_OBJECT_ID) return dispatch_output(server, call, request, true);
	return false;
}

bool dm_supervisor_init(struct device_server* server, cap_id_t self_cap) {
	server->supervisor = (struct dm_driver_supervisor){
		.self_cap    = self_cap,
		.control_cap = CAP_ID_INVALID,
		.stdout_cap  = CAP_ID_INVALID,
		.stderr_cap  = CAP_ID_INVALID,
	};
	if (cap_publish(server->endpoint,
	                DM_SUPERVISOR_CONTROL_OBJECT_ID,
	                server->pid,
	                CAP_CALL,
	                &server->supervisor.control_cap) != SYSCALL_STATUS_OK)
		goto fail;
	if (cap_publish(server->endpoint,
	                DM_SUPERVISOR_STDOUT_OBJECT_ID,
	                server->pid,
	                CAP_CALL | CAP_WRITE | CAP_DELEGATE,
	                &server->supervisor.stdout_cap) != SYSCALL_STATUS_OK)
		goto fail;
	if (cap_publish(server->endpoint,
	                DM_SUPERVISOR_STDERR_OBJECT_ID,
	                server->pid,
	                CAP_CALL | CAP_WRITE | CAP_DELEGATE,
	                &server->supervisor.stderr_cap) != SYSCALL_STATUS_OK)
		goto fail;
	return true;
fail:
	dm_supervisor_deinit(server);
	return false;
}

void dm_supervisor_deinit(struct device_server* server) {
	if (server == NULL) return;
	if (server->endpoint != CHANNEL_ID_INVALID) {
		(void)cap_unpublish(server->endpoint, DM_SUPERVISOR_STDERR_OBJECT_ID);
		(void)cap_unpublish(server->endpoint, DM_SUPERVISOR_STDOUT_OBJECT_ID);
		(void)cap_unpublish(server->endpoint, DM_SUPERVISOR_CONTROL_OBJECT_ID);
	}
	while (server->supervisor.bindings != NULL) {
		struct dm_driver_binding* next = server->supervisor.bindings->next;
		if (server->supervisor.bindings->process_cap != CAP_ID_INVALID)
			(void)process_kill(server->supervisor.bindings->process_cap, PROCESS_EXIT_SYSTEM_UNKNOWN);
		free_binding(server->supervisor.bindings);
		server->supervisor.bindings = next;
	}
	if (server->supervisor.self_cap != CAP_ID_INVALID) (void)cap_drop(server->supervisor.self_cap);
	server->supervisor = (struct dm_driver_supervisor){
		.self_cap    = CAP_ID_INVALID,
		.control_cap = CAP_ID_INVALID,
		.stdout_cap  = CAP_ID_INVALID,
		.stderr_cap  = CAP_ID_INVALID,
	};
}

void dm_supervisor_device_committed(struct device_server* server, struct dm_device* device) {
	if (server == NULL || device == NULL || device->binding != NULL) return;
	struct dm_driver* driver = match_driver(server, device, NULL);
	if (driver == NULL) return;
	struct dm_driver_binding* binding = calloc(1u, sizeof(*binding));
	if (binding == NULL) {
		printf("device-manager: unable to allocate binding for device %llu\n", (unsigned long long)device->id);
		return;
	}
	*binding = (struct dm_driver_binding){
		.device      = device,
		.driver      = driver,
		.state       = DM_DRIVER_UNBOUND,
		.process_id  = PROCESS_PID_INVALID,
		.process_cap = CAP_ID_INVALID,
		.next        = server->supervisor.bindings,
	};
	server->supervisor.bindings = binding;
	device->binding             = binding;
	binding_launch(server, binding);
}

void dm_supervisor_reconcile(struct device_server* server) {
	if (server == NULL) return;
	for (struct dm_device* device = server->state.devices; device != NULL; device = device->next)
		dm_supervisor_device_committed(server, device);
}

bool dm_supervisor_authorize_child(struct device_server* server, const struct dm_device* parent, process_id_t caller,
                                   struct dm_driver_binding** out_binding, uint64_t* out_generation) {
	if (server == NULL || parent == NULL || out_binding == NULL || out_generation == NULL) return false;
	struct dm_driver_binding* binding = parent->binding;
	if (binding == NULL || binding->state != DM_DRIVER_RUNNING || binding->process_id != caller) return false;
	*out_binding    = binding;
	*out_generation = binding->generation;
	return true;
}
