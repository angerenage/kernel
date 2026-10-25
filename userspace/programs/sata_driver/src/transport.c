#include "transport.h"

#include <base/address_space.h>
#include <base/memory.h>
#include <base/process.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stddef.h>
#include <system/capability.h>
#include <system/memory.h>
#include <system/process.h>

static bool transport_open(cap_id_t root_cap, struct ahci_transport_open_response* out_response) {
	const struct ahci_transport_open_request request = {.header = {.op = AHCI_TRANSPORT_OP_OPEN}};
	size_t                                   response_size;
	syscall_status_t                         status =
		cap_call(root_cap, &request, sizeof(request), out_response, sizeof(*out_response), &response_size);
	return status == SYSCALL_STATUS_OK && response_size == sizeof(*out_response) &&
	       out_response->session_cap != CAP_ID_INVALID;
}

bool sata_transport_initialize(cap_id_t device_cap, struct sata_transport* out_transport) {
	struct ahci_transport_open_response response = {.session_cap = CAP_ID_INVALID};

	if (out_transport == NULL) return false;
	*out_transport = (struct sata_transport){.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	if (device_resource_acquire(device_cap,
	                            AHCI_TRANSPORT_RESOURCE,
	                            sizeof(AHCI_TRANSPORT_RESOURCE) - 1u,
	                            AHCI_TRANSPORT_DRIVER_RIGHTS,
	                            &out_transport->root_cap) != SYSCALL_STATUS_OK ||
	    !transport_open(out_transport->root_cap, &response)) {
		sata_transport_deinitialize(out_transport);
		return false;
	}
	out_transport->session_cap = response.session_cap;
	if (response.kind != AHCI_DEVICE_SATA) {
		sata_transport_deinitialize(out_transport);
		return false;
	}
	out_transport->port              = response.port;
	out_transport->max_transfer_size = response.max_transfer_size;
	return true;
}

bool sata_buffer_allocate(const struct sata_transport* transport, uint32_t size, struct sata_buffer* out_buffer) {
	const struct ahci_transport_allocate_buffer_request request  = {.header = {.op = AHCI_TRANSPORT_OP_ALLOCATE_BUFFER},
	                                                                .size   = size};
	struct ahci_transport_allocate_buffer_response      response = {0};
	struct address_space_map_response                   mapping  = {.mapping_cap = CAP_ID_INVALID};
	struct self_info                                    self;
	size_t                                              response_size;

	if (transport == NULL || out_buffer == NULL || transport->session_cap == CAP_ID_INVALID || size == 0u ||
	    size > transport->max_transfer_size)
		return false;
	*out_buffer = (struct sata_buffer){
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	if (cap_call(transport->session_cap, &request, sizeof(request), &response, sizeof(response), &response_size) !=
	        SYSCALL_STATUS_OK ||
	    response_size != sizeof(response) || response.memory_cap == CAP_ID_INVALID || response.size < size)
		return false;
	*out_buffer = (struct sata_buffer){.id                = response.buffer_id,
	                                   .memory_cap        = response.memory_cap,
	                                   .address_space_cap = CAP_ID_INVALID,
	                                   .mapping_cap       = CAP_ID_INVALID,
	                                   .size              = response.size};
	if (process_self_info(&self) != SYSCALL_STATUS_OK) return false;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	out_buffer->address_space_cap = self.address_space_cap;
	if (out_buffer->address_space_cap == CAP_ID_INVALID ||
	    address_space_map(out_buffer->address_space_cap,
	                      out_buffer->memory_cap,
	                      MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                      0u,
	                      0u,
	                      0u,
	                      0u,
	                      &mapping) != SYSCALL_STATUS_OK ||
	    mapping.mapping_cap == CAP_ID_INVALID || mapping.address == 0u)
		return false;
	out_buffer->mapping_cap = mapping.mapping_cap;
	out_buffer->address     = (uint8_t*)mapping.address;
	return true;
}

bool sata_transport_execute(const struct sata_transport*                 transport,
                            const struct ahci_transport_execute_request* request,
                            struct ahci_transport_execute_response*      out_response) {
	size_t response_size;

	if (transport == NULL || request == NULL || out_response == NULL || transport->session_cap == CAP_ID_INVALID)
		return false;
	return cap_call(transport->session_cap,
	                request,
	                sizeof(*request),
	                out_response,
	                sizeof(*out_response),
	                &response_size) == SYSCALL_STATUS_OK &&
	       response_size == sizeof(*out_response);
}

void sata_buffer_release(const struct sata_transport* transport, struct sata_buffer* buffer) {
	if (buffer == NULL) return;
	if (buffer->mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(buffer->mapping_cap);
	if (buffer->address_space_cap != CAP_ID_INVALID) (void)cap_drop(buffer->address_space_cap);
	if (buffer->memory_cap != CAP_ID_INVALID) (void)cap_drop(buffer->memory_cap);
	if (transport != NULL && transport->session_cap != CAP_ID_INVALID && buffer->id != 0u) {
		const struct ahci_transport_release_buffer_request request = {
			.header = {.op = AHCI_TRANSPORT_OP_RELEASE_BUFFER}, .buffer_id = buffer->id};
		size_t response_size;
		(void)cap_call(transport->session_cap, &request, sizeof(request), NULL, 0u, &response_size);
	}
	*buffer = (struct sata_buffer){
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
}

void sata_transport_deinitialize(struct sata_transport* transport) {
	if (transport == NULL) return;
	if (transport->session_cap != CAP_ID_INVALID) (void)cap_drop(transport->session_cap);
	if (transport->root_cap != CAP_ID_INVALID) (void)cap_drop(transport->root_cap);
	*transport = (struct sata_transport){.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
}
