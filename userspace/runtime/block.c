#include <base/address_space.h>
#include <base/memory.h>
#include <base/process.h>
#include <protocol/device.h>
#include <runtime/block.h>
#include <runtime/device.h>
#include <stddef.h>
#include <system/capability.h>
#include <system/memory.h>
#include <system/process.h>

static syscall_status_t open_session(cap_id_t root_cap, struct block_open_response* out_response) {
	const struct block_open_request request = {.header = {.op = BLOCK_OP_OPEN}};
	size_t                          response_size;
	syscall_status_t                status =
		cap_call(root_cap, &request, sizeof(request), out_response, sizeof(*out_response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == sizeof(*out_response) && out_response->session_cap != CAP_ID_INVALID &&
	               out_response->block_count != 0u && out_response->block_size != 0u &&
	               out_response->max_transfer_blocks != 0u &&
	               out_response->max_transfer_blocks <= UINT32_MAX / out_response->block_size &&
	               (out_response->flags &
	                ~(uint32_t)(BLOCK_DEVICE_WRITABLE | BLOCK_DEVICE_FLUSH_SUPPORTED | BLOCK_DEVICE_REMOVABLE)) == 0u &&
	               out_response->reserved == 0u
			   ? SYSCALL_STATUS_OK
			   : SYSCALL_STATUS_FAILED;
}

static syscall_status_t block_device_open_with_rights(cap_id_t device_cap, cap_rights_t rights,
                                                      struct block_device* out_device) {
	struct block_open_response response = {.session_cap = CAP_ID_INVALID};
	syscall_status_t           status;

	if (out_device == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_device = (struct block_device){.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	status =
		device_resource_acquire(device_cap, BLOCK_RESOURCE, sizeof(BLOCK_RESOURCE) - 1u, rights, &out_device->root_cap);
	if (status == SYSCALL_STATUS_OK) status = open_session(out_device->root_cap, &response);
	if (status != SYSCALL_STATUS_OK) {
		block_device_close(out_device);
		return status;
	}
	out_device->session_cap         = response.session_cap;
	out_device->block_count         = response.block_count;
	out_device->block_size          = response.block_size;
	out_device->max_transfer_blocks = response.max_transfer_blocks;
	out_device->flags               = response.flags;
	return SYSCALL_STATUS_OK;
}

syscall_status_t block_device_open(cap_id_t device_cap, struct block_device* out_device) {
	return block_device_open_with_rights(device_cap, BLOCK_DRIVER_RIGHTS, out_device);
}

syscall_status_t block_device_open_read_only(cap_id_t device_cap, struct block_device* out_device) {
	return block_device_open_with_rights(device_cap, BLOCK_READER_RIGHTS, out_device);
}

void block_device_close(struct block_device* device) {
	if (device == NULL) return;
	if (device->session_cap != CAP_ID_INVALID) (void)cap_drop(device->session_cap);
	if (device->root_cap != CAP_ID_INVALID) (void)cap_drop(device->root_cap);
	*device = (struct block_device){.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
}

syscall_status_t block_buffer_allocate(const struct block_device* device, uint32_t size,
                                       struct block_buffer* out_buffer) {
	const struct block_allocate_buffer_request request  = {.header = {.op = BLOCK_OP_ALLOCATE_BUFFER}, .size = size};
	struct block_allocate_buffer_response      response = {.memory_cap = CAP_ID_INVALID};
	struct address_space_map_response          mapping  = {.mapping_cap = CAP_ID_INVALID};
	struct self_info                           self     = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	size_t           response_size;
	syscall_status_t status;

	if (device == NULL || out_buffer == NULL || device->session_cap == CAP_ID_INVALID || size == 0u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_buffer = (struct block_buffer){
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	status = cap_call(device->session_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.memory_cap == CAP_ID_INVALID || response.buffer_id == 0u ||
	    response.size < size || response.reserved != 0u) {
		if (response.memory_cap != CAP_ID_INVALID) (void)cap_drop(response.memory_cap);
		return SYSCALL_STATUS_FAILED;
	}
	*out_buffer = (struct block_buffer){.id                = response.buffer_id,
	                                    .memory_cap        = response.memory_cap,
	                                    .address_space_cap = CAP_ID_INVALID,
	                                    .mapping_cap       = CAP_ID_INVALID,
	                                    .size              = response.size};
	status      = process_self_info(&self);
	if (status == SYSCALL_STATUS_OK) {
		out_buffer->address_space_cap = self.address_space_cap;
		self.address_space_cap        = CAP_ID_INVALID;
		status                        = address_space_map(out_buffer->address_space_cap,
		                                                  out_buffer->memory_cap,
		                                                  MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
		                                                  0u,
		                                                  0u,
		                                                  0u,
		                                                  0u,
		                                                  &mapping);
	}
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (status == SYSCALL_STATUS_OK && (mapping.mapping_cap == CAP_ID_INVALID || mapping.address == 0u))
		status = SYSCALL_STATUS_FAILED;
	if (status != SYSCALL_STATUS_OK) {
		block_buffer_release(device, out_buffer);
		return status;
	}
	out_buffer->mapping_cap = mapping.mapping_cap;
	out_buffer->address     = (uint8_t*)mapping.address;
	return SYSCALL_STATUS_OK;
}

void block_buffer_release(const struct block_device* device, struct block_buffer* buffer) {
	if (buffer == NULL) return;
	if (buffer->mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(buffer->mapping_cap);
	if (buffer->address_space_cap != CAP_ID_INVALID) (void)cap_drop(buffer->address_space_cap);
	if (buffer->memory_cap != CAP_ID_INVALID) (void)cap_drop(buffer->memory_cap);
	if (device != NULL && device->session_cap != CAP_ID_INVALID && buffer->id != 0u) {
		const struct block_release_buffer_request request = {.header    = {.op = BLOCK_OP_RELEASE_BUFFER},
		                                                     .buffer_id = buffer->id};
		size_t                                    response_size;
		(void)cap_call(device->session_cap, &request, sizeof(request), NULL, 0u, &response_size);
	}
	*buffer = (struct block_buffer){
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
}

static syscall_status_t transfer(const struct block_device* device, struct block_buffer* buffer, uint32_t buffer_offset,
                                 uint64_t block_index, uint32_t block_count, enum block_op operation) {
	const struct block_transfer_request request = {.header        = {.op = operation},
	                                               .block_count   = block_count,
	                                               .block_index   = block_index,
	                                               .buffer_id     = buffer == NULL ? 0u : buffer->id,
	                                               .buffer_offset = buffer_offset};
	struct block_transfer_response      response;
	size_t                              response_size;
	syscall_status_t                    status;

	if (device == NULL || buffer == NULL || device->session_cap == CAP_ID_INVALID || block_count == 0u ||
	    block_count > device->max_transfer_blocks || block_index >= device->block_count ||
	    block_count > device->block_count - block_index || block_count > UINT32_MAX / device->block_size)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	uint32_t size = block_count * device->block_size;
	if (buffer_offset > buffer->size || size > buffer->size - buffer_offset) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = cap_call(device->session_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == sizeof(response) && response.reserved == 0u && response.transferred_blocks == block_count
			   ? SYSCALL_STATUS_OK
			   : SYSCALL_STATUS_FAILED;
}

syscall_status_t block_read(const struct block_device* device, struct block_buffer* buffer, uint32_t buffer_offset,
                            uint64_t block_index, uint32_t block_count) {
	return transfer(device, buffer, buffer_offset, block_index, block_count, BLOCK_OP_READ);
}

syscall_status_t block_write(const struct block_device* device, struct block_buffer* buffer, uint32_t buffer_offset,
                             uint64_t block_index, uint32_t block_count) {
	if (device == NULL || (device->flags & BLOCK_DEVICE_WRITABLE) == 0u) return SYSCALL_STATUS_DENIED;
	return transfer(device, buffer, buffer_offset, block_index, block_count, BLOCK_OP_WRITE);
}

syscall_status_t block_flush(const struct block_device* device) {
	const struct block_flush_request request = {.header = {.op = BLOCK_OP_FLUSH}};
	size_t                           response_size;

	if (device == NULL || device->session_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	if ((device->flags & BLOCK_DEVICE_FLUSH_SUPPORTED) == 0u) return SYSCALL_STATUS_UNAVAILABLE;
	syscall_status_t status = cap_call(device->session_cap, &request, sizeof(request), NULL, 0u, &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}
