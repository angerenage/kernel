#pragma once

/* Controller-independent random-access block-device interface. */

#include <base/cap.h>
#include <base/syscall.h>
#include <stdint.h>

#define BLOCK_RESOURCE "block"
#define BLOCK_COMPATIBLE_DEVICE "block:device"
#define BLOCK_COMPATIBLE_DISK "block:disk"

#define BLOCK_DRIVER_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE))

enum block_op {
	BLOCK_OP_OPEN = 0u,
	BLOCK_OP_ALLOCATE_BUFFER,
	BLOCK_OP_READ,
	BLOCK_OP_WRITE,
	BLOCK_OP_FLUSH,
	BLOCK_OP_RELEASE_BUFFER,
};

enum block_device_flag {
	BLOCK_DEVICE_WRITABLE        = 1u << 0u,
	BLOCK_DEVICE_FLUSH_SUPPORTED = 1u << 1u,
	BLOCK_DEVICE_REMOVABLE       = 1u << 2u,
};

struct block_request_header {
	enum block_op op;
};

/* Open one client session and query immutable device geometry. */
struct block_open_request {
	struct block_request_header header;
};

struct block_open_response {
	cap_id_t session_cap;
	uint64_t block_count;
	uint32_t block_size;
	uint32_t max_transfer_blocks;
	uint32_t flags;
	uint32_t reserved;
};

/* Allocate storage that is directly usable by the provider's DMA engine. */
struct block_allocate_buffer_request {
	struct block_request_header header;
	uint32_t                    size;
};

struct block_allocate_buffer_response {
	uint64_t buffer_id;
	cap_id_t memory_cap;
	uint32_t size;
	uint32_t reserved;
};

/* Transfer complete logical blocks between a device and a session buffer. */
struct block_transfer_request {
	struct block_request_header header;
	uint32_t                    block_count;
	uint64_t                    block_index;
	uint64_t                    buffer_id;
	uint32_t                    buffer_offset;
	uint32_t                    reserved;
};

struct block_transfer_response {
	uint32_t transferred_blocks;
	uint32_t reserved;
};

/* Commit all writes completed before this request. */
struct block_flush_request {
	struct block_request_header header;
};

/* Release one session buffer. */
struct block_release_buffer_request {
	struct block_request_header header;
	uint32_t                    reserved;
	uint64_t                    buffer_id;
};

/* One opened block-device session. */
struct block_device {
	cap_id_t root_cap;
	cap_id_t session_cap;
	uint64_t block_count;
	uint32_t block_size;
	uint32_t max_transfer_blocks;
	uint32_t flags;
};

/* One provider-allocated buffer mapped into the client's address space. */
struct block_buffer {
	uint64_t id;
	cap_id_t memory_cap;
	cap_id_t address_space_cap;
	cap_id_t mapping_cap;
	uint8_t* address;
	uint32_t size;
};

/* Acquire a device's block resource and open an isolated session. */
syscall_status_t block_device_open(cap_id_t device_cap, struct block_device* out_device);

/* Close a session after releasing all of its buffers. */
void block_device_close(struct block_device* device);

/* Allocate and map a buffer suitable for this block provider. */
syscall_status_t block_buffer_allocate(const struct block_device* device, uint32_t size,
                                       struct block_buffer* out_buffer);

/* Unmap and release a provider-owned buffer. */
void block_buffer_release(const struct block_device* device, struct block_buffer* buffer);

/* Read complete logical blocks into a provider-owned buffer. */
syscall_status_t block_read(const struct block_device* device, struct block_buffer* buffer, uint32_t buffer_offset,
                            uint64_t block_index, uint32_t block_count);

/* Write complete logical blocks from a provider-owned buffer. */
syscall_status_t block_write(const struct block_device* device, struct block_buffer* buffer, uint32_t buffer_offset,
                             uint64_t block_index, uint32_t block_count);

/* Commit writes completed before this call. */
syscall_status_t block_flush(const struct block_device* device);
