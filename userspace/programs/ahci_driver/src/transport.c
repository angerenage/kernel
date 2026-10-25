#include "transport.h"

#include <base/channel.h>
#include <base/memory.h>
#include <base/process.h>
#include <base/signal.h>
#include <protocol/ahci.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#include "command.h"
#include "dma.h"
#include "port.h"
#include "registers.h"

#define AHCI_MAX_SESSION_BUFFERS 8u
#define AHCI_SESSION_OBJECT_BASE 0x100000000ull
#define AHCI_PORT_PROVIDER_RIGHTS (AHCI_TRANSPORT_DRIVER_RIGHTS | CAP_DELEGATE)
#define AHCI_SESSION_RIGHTS AHCI_TRANSPORT_DRIVER_RIGHTS
#define AHCI_CLIENT_MEMORY_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE))

struct ahci_client_buffer {
	bool                   used;
	uint64_t               id;
	cap_id_t               client_cap;
	struct ahci_dma_buffer dma;
};

struct ahci_session {
	bool                      active;
	uint64_t                  object_id;
	process_id_t              caller;
	struct ahci_port*         port;
	uint64_t                  next_buffer_id;
	struct ahci_client_buffer buffers[AHCI_MAX_SESSION_BUFFERS];
};

struct ahci_transport {
	channel_id_t                   endpoint;
	cap_id_t                       activity_cap;
	process_id_t                   pid;
	cap_id_t                       parent_device_cap;
	const struct ahci_dma_context* dma;
	struct ahci_port               ports[AHCI_PORT_COUNT];
	struct ahci_session            sessions[AHCI_PORT_COUNT];
	uint32_t                       port_count;
	uint64_t                       next_session_id;
};

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply(const struct cap_request* call, const void* data, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, data, size, status) == SYSCALL_STATUS_OK;
}

static struct ahci_port* find_port(struct ahci_transport* transport, uint64_t object_id) {
	for (uint32_t index = 0u; index < transport->port_count; index++)
		if (transport->ports[index].object_id == object_id) return &transport->ports[index];
	return NULL;
}

static struct ahci_session* find_session(struct ahci_transport* transport, uint64_t object_id) {
	for (uint32_t index = 0u; index < AHCI_PORT_COUNT; index++)
		if (transport->sessions[index].active && transport->sessions[index].object_id == object_id)
			return &transport->sessions[index];
	return NULL;
}

static struct ahci_session* port_session(struct ahci_transport* transport, const struct ahci_port* port) {
	for (uint32_t index = 0u; index < AHCI_PORT_COUNT; index++)
		if (transport->sessions[index].active && transport->sessions[index].port == port)
			return &transport->sessions[index];
	return NULL;
}

static void release_buffer(struct ahci_client_buffer* buffer) {
	if (!buffer->used) return;
	if (buffer->client_cap != CAP_ID_INVALID) (void)cap_revoke(buffer->client_cap, 0u);
	ahci_dma_buffer_release(&buffer->dma);
	*buffer = (struct ahci_client_buffer){.client_cap = CAP_ID_INVALID};
}

static void release_session(struct ahci_session* session) {
	if (session == NULL || !session->active) return;
	for (size_t index = 0u; index < AHCI_MAX_SESSION_BUFFERS; index++) release_buffer(&session->buffers[index]);
	*session = (struct ahci_session){0};
}

static bool dispatch_open(struct ahci_transport* transport, struct ahci_port* port, const struct cap_request* call) {
	struct ahci_transport_open_response response = {0};
	struct ahci_session*                session  = NULL;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct ahci_transport_open_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (port_session(transport, port) != NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	for (size_t index = 0u; index < AHCI_PORT_COUNT; index++)
		if (!transport->sessions[index].active) {
			session = &transport->sessions[index];
			break;
		}
	if (session == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*session = (struct ahci_session){
		.active         = true,
		.object_id      = AHCI_SESSION_OBJECT_BASE + transport->next_session_id++,
		.caller         = call->caller,
		.port           = port,
		.next_buffer_id = 1u,
	};
	for (size_t index = 0u; index < AHCI_MAX_SESSION_BUFFERS; index++)
		session->buffers[index].client_cap = CAP_ID_INVALID;
	response.kind              = port->kind;
	response.port              = port->index;
	response.max_transfer_size = AHCI_TRANSPORT_MAX_TRANSFER;
	if (cap_publish(
			transport->endpoint, session->object_id, call->caller, AHCI_SESSION_RIGHTS, &response.session_cap) !=
	    SYSCALL_STATUS_OK) {
		release_session(session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(transport->endpoint, session->object_id);
	release_session(session);
	return false;
}

static bool dispatch_allocate(struct ahci_transport* transport, struct ahci_session* session,
                              const struct cap_request* call, const void* data) {
	struct ahci_transport_allocate_buffer_request  request;
	struct ahci_transport_allocate_buffer_response response = {0};
	struct ahci_client_buffer*                     buffer   = NULL;

	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u || request.size == 0u || request.size > AHCI_TRANSPORT_MAX_TRANSFER)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	for (size_t index = 0u; index < AHCI_MAX_SESSION_BUFFERS; index++)
		if (!session->buffers[index].used) {
			buffer = &session->buffers[index];
			break;
		}
	if (buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	*buffer = (struct ahci_client_buffer){
		.used       = true,
		.id         = session->next_buffer_id++,
		.client_cap = CAP_ID_INVALID,
		.dma = {.memory_cap = CAP_ID_INVALID, .cpu_mapping_cap = CAP_ID_INVALID, .device_mapping_cap = CAP_ID_INVALID},
	};
	if (!ahci_dma_buffer_allocate(
			transport->dma, request.size, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, &buffer->dma) ||
	    buffer->dma.size > UINT32_MAX ||
	    cap_delegate(buffer->dma.memory_cap, call->caller, AHCI_CLIENT_MEMORY_RIGHTS, &buffer->client_cap) !=
	        SYSCALL_STATUS_OK) {
		release_buffer(buffer);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	response.buffer_id  = buffer->id;
	response.memory_cap = buffer->client_cap;
	response.size       = (uint32_t)buffer->dma.size;
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	release_buffer(buffer);
	return false;
}

static struct ahci_client_buffer* find_buffer(struct ahci_session* session, uint64_t id) {
	for (size_t index = 0u; index < AHCI_MAX_SESSION_BUFFERS; index++)
		if (session->buffers[index].used && session->buffers[index].id == id) return &session->buffers[index];
	return NULL;
}

static bool dispatch_execute(struct ahci_session* session, const struct cap_request* call, const void* data) {
	struct ahci_transport_execute_request  request;
	struct ahci_transport_execute_response response = {0};
	struct ahci_client_buffer*             buffer   = NULL;

	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved[0] != 0u || request.reserved[1] != 0u ||
	    (request.packet_size != 0u && request.packet_size != 12u && request.packet_size != 16u) ||
	    request.direction > AHCI_DATA_TO_DEVICE || request.transfer_size > AHCI_TRANSPORT_MAX_TRANSFER ||
	    ((request.direction == AHCI_DATA_NONE) != (request.transfer_size == 0u)))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.transfer_size != 0u) {
		buffer = find_buffer(session, request.buffer_id);
		if (buffer == NULL || request.buffer_offset > buffer->dma.size ||
		    request.transfer_size > buffer->dma.size - request.buffer_offset)
			return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	if (!ahci_command_execute(session->port, &request, buffer == NULL ? NULL : &buffer->dma, &response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch_release(struct ahci_session* session, const struct cap_request* call, const void* data) {
	struct ahci_transport_release_buffer_request request;
	struct ahci_client_buffer*                   buffer;

	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	buffer = find_buffer(session, request.buffer_id);
	if (buffer == NULL) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	release_buffer(buffer);
	return reply(call, NULL, 0u, SYSCALL_STATUS_OK);
}

static bool dispatch(struct ahci_transport* transport, const struct cap_request* call, const void* data) {
	struct ahci_transport_request_header header;
	struct ahci_port*                    port;
	struct ahci_session*                 session;

	if (data == NULL || call->request_size < sizeof(header)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	port = find_port(transport, call->object_id);
	if (port != NULL)
		return header.op == AHCI_TRANSPORT_OP_OPEN ? dispatch_open(transport, port, call)
		                                           : reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	session = find_session(transport, call->object_id);
	if (session == NULL || session->caller != call->caller) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	switch (header.op) {
	case AHCI_TRANSPORT_OP_ALLOCATE_BUFFER:
		return dispatch_allocate(transport, session, call, data);
	case AHCI_TRANSPORT_OP_EXECUTE:
		return dispatch_execute(session, call, data);
	case AHCI_TRANSPORT_OP_RELEASE_BUFFER:
		return dispatch_release(session, call, data);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool create_child(cap_id_t parent, const struct ahci_port* port) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	char                  name[24];
	const char*           specific = port->kind == AHCI_DEVICE_SATAPI ? AHCI_COMPATIBLE_SATAPI : AHCI_COMPATIBLE_SATA;
	int                   name_size;
	syscall_status_t      status;

	name_size = snprintf(name, sizeof(name), "ahci_port_%u", port->index);
	if (name_size < 0 || (size_t)name_size >= sizeof(name)) return false;
	status = device_builder_begin_child(parent, &builder);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, name, (size_t)name_size);
	if (status == SYSCALL_STATUS_OK) status = device_builder_add_compatible(&builder, specific, strlen(specific));
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, AHCI_COMPATIBLE_DEVICE, sizeof(AHCI_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(&builder,
		                                     AHCI_TRANSPORT_RESOURCE,
		                                     sizeof(AHCI_TRANSPORT_RESOURCE) - 1u,
		                                     port->transport_cap,
		                                     AHCI_TRANSPORT_DRIVER_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static bool add_port(struct ahci_transport* transport, volatile struct ahci_port_registers* registers, uint32_t index,
                     enum ahci_device_kind kind, bool address_64bit) {
	if (transport->port_count == AHCI_PORT_COUNT) return false;
	struct ahci_port* port = &transport->ports[transport->port_count];
	if (!ahci_port_initialize(port, registers, index, kind, address_64bit, transport->dma)) return false;
	if (cap_publish(
			transport->endpoint, port->object_id, transport->pid, AHCI_PORT_PROVIDER_RIGHTS, &port->transport_cap) !=
	    SYSCALL_STATUS_OK) {
		fprintf(stderr, "ahci: port %u could not publish its transport capability\n", index);
		ahci_port_deinitialize(port);
		return false;
	}
	if (!create_child(transport->parent_device_cap, port)) {
		fprintf(stderr, "ahci: port %u child-device creation failed\n", index);
		(void)cap_unpublish(transport->endpoint, port->object_id);
		ahci_port_deinitialize(port);
		return false;
	}
	port->published = true;
	transport->port_count++;
	printf("ahci: port %u published as %s device\n", index, kind == AHCI_DEVICE_SATAPI ? "SATAPI" : "SATA");
	return true;
}

static void cleanup(struct ahci_transport* transport) {
	for (size_t index = 0u; index < AHCI_PORT_COUNT; index++) {
		if (transport->sessions[index].active) {
			(void)cap_unpublish(transport->endpoint, transport->sessions[index].object_id);
			release_session(&transport->sessions[index]);
		}
	}
	for (uint32_t index = transport->port_count; index != 0u; index--) {
		struct ahci_port* port = &transport->ports[index - 1u];
		if (port->published) (void)cap_unpublish(transport->endpoint, port->object_id);
		ahci_port_deinitialize(port);
	}
	if (transport->activity_cap != CAP_ID_INVALID) (void)cap_drop(transport->activity_cap);
	if (transport->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(transport->endpoint);
}

static bool serve(struct ahci_transport* transport) {
	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(transport->endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK) return false;
			if (received && !dispatch(transport, &call, request_buffer)) return false;
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(transport->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return false;
			if (received && event.type == CHANNEL_EVENT_CAP_ZERO_GRANTS) {
				struct ahci_session* session = find_session(transport, event.object_id);
				if (session != NULL) {
					(void)cap_unpublish_if_unused(transport->endpoint, session->object_id);
					release_session(session);
				}
			}
		} while (received);
		struct signal_message activity;
		status = signal_wait(transport->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return false;
	}
}

bool ahci_transport_run(cap_id_t device_cap, volatile struct ahci_host_registers* registers, size_t bar_size,
                        uint32_t capabilities, uint32_t implemented, const struct ahci_dma_context* dma) {
	struct ahci_transport transport = {
		.endpoint          = CHANNEL_ID_INVALID,
		.activity_cap      = CAP_ID_INVALID,
		.parent_device_cap = device_cap,
		.dma               = dma,
		.next_session_id   = 1u,
	};
	struct self_info self;
	uint32_t         port_limit = (capabilities & AHCI_CAP_NP_MASK) + 1u;
	bool             success    = false;

	if (registers == NULL || dma == NULL || port_limit > AHCI_PORT_COUNT ||
	    (registers->global_host_control & AHCI_GHC_AE) == 0u || process_self_info(&self) != SYSCALL_STATUS_OK)
		return false;
	transport.pid = self.pid;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (channel_create(&transport.endpoint, &transport.activity_cap) != SYSCALL_STATUS_OK) goto done;
	for (uint32_t index = 0u; index < port_limit; index++) {
		size_t offset = AHCI_PORT_REGISTERS_OFFSET + (size_t)index * AHCI_PORT_REGISTERS_SIZE;
		if ((implemented & (1u << index)) == 0u) continue;
		if (offset > bar_size || sizeof(struct ahci_port_registers) > bar_size - offset) goto done;
		volatile struct ahci_port_registers* port =
			(volatile struct ahci_port_registers*)((volatile uint8_t*)registers + offset);
		if ((port->sata_status & AHCI_PORT_SSTS_DET_MASK) != 3u) continue;
		if (port->signature == AHCI_PORT_SIGNATURE_ATA) {
			if (!add_port(&transport, port, index, AHCI_DEVICE_SATA, (capabilities & AHCI_CAP_S64A) != 0u)) goto done;
		}
		else if (port->signature == AHCI_PORT_SIGNATURE_ATAPI) {
			if (!add_port(&transport, port, index, AHCI_DEVICE_SATAPI, (capabilities & AHCI_CAP_S64A) != 0u)) goto done;
		}
	}
	if (transport.port_count == 0u) {
		printf("ahci: no supported devices attached\n");
	}
	success = serve(&transport);

done:
	cleanup(&transport);
	return success;
}
