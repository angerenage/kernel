#include "bot.h"

#include <protocol/device.h>
#include <protocol/usb.h>
#include <runtime/device.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/time.h>

#define USB_MSC_CLASS 0x08u
#define USB_MSC_SCSI_SUBCLASS 0x06u
#define USB_MSC_BULK_ONLY_PROTOCOL 0x50u
#define USB_ENDPOINT_BULK 0x02u
#define USB_MSC_CBW_SIZE 31u
#define USB_MSC_CSW_SIZE 13u
#define USB_MSC_CBW_SIGNATURE 0x43425355u
#define USB_MSC_CSW_SIGNATURE 0x53425355u
#define USB_MSC_TRANSFER_LIMIT (128u * 1024u)

static void put_le32(uint8_t* data, uint32_t value) {
	for (unsigned i = 0u; i < 4u; i++) data[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t get_le32(const uint8_t* data) {
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) | ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static void put_be16(uint8_t* data, uint16_t value) {
	data[0] = (uint8_t)(value >> 8u);
	data[1] = (uint8_t)value;
}

static void put_be32(uint8_t* data, uint32_t value) {
	for (unsigned i = 0u; i < 4u; i++) data[i] = (uint8_t)(value >> (24u - 8u * i));
}

static void put_be64(uint8_t* data, uint64_t value) {
	for (unsigned i = 0u; i < 8u; i++) data[i] = (uint8_t)(value >> (56u - 8u * i));
}

static uint32_t get_be32(const uint8_t* data) {
	return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u) | ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static uint64_t get_be64(const uint8_t* data) {
	uint64_t value = 0u;
	for (unsigned i = 0u; i < 8u; i++) value = (value << 8u) | data[i];
	return value;
}

static bool transfer(struct usb_msc_device* device, uint8_t endpoint, void* data, size_t length, size_t* transferred) {
	bool in = (endpoint & 0x80u) != 0u;
	if (length > USB_TRANSPORT_MAX_TRANSFER || (length != 0u && data == NULL)) return false;
	size_t request_size                            = sizeof(struct usb_transport_transfer_request) + (in ? 0u : length);
	struct usb_transport_transfer_request* request = malloc(request_size);
	if (request == NULL) return false;
	*request = (struct usb_transport_transfer_request){
		.header = {.op = USB_TRANSPORT_OP_TRANSFER}, .endpoint = endpoint, .length = (uint32_t)length};
	if (!in && length != 0u) memcpy(request->data, data, length);
	size_t   response_capacity = sizeof(struct usb_transport_transfer_response) + (in ? length : 0u);
	uint8_t* response          = malloc(response_capacity);
	if (response == NULL) {
		free(request);
		return false;
	}
	size_t           response_size = 0u;
	syscall_status_t status =
		cap_call(device->transport_cap, request, request_size, response, response_capacity, &response_size);
	free(request);
	if (status != SYSCALL_STATUS_OK || response_size < sizeof(struct usb_transport_transfer_response)) {
		free(response);
		return false;
	}
	struct usb_transport_transfer_response header;
	memcpy(&header, response, sizeof(header));
	if (header.transferred > length || response_size != sizeof(header) + (in ? header.transferred : 0u)) {
		free(response);
		return false;
	}
	if (in && header.transferred != 0u) memcpy(data, response + sizeof(header), header.transferred);
	if (transferred != NULL) *transferred = header.transferred;
	free(response);
	return true;
}

static bool transfer_all(struct usb_msc_device* device, uint8_t endpoint, void* data, size_t length) {
	uint8_t* bytes = data;
	for (size_t offset = 0u; offset < length;) {
		size_t chunk = length - offset;
		if (chunk > USB_TRANSPORT_MAX_TRANSFER) chunk = USB_TRANSPORT_MAX_TRANSFER;
		size_t transferred = 0u;
		if (!transfer(device, endpoint, bytes + offset, chunk, &transferred) || transferred == 0u) return false;
		offset += transferred;
		if (transferred != chunk && offset != length) return false;
	}
	return true;
}

static bool command(struct usb_msc_device* device, const uint8_t* cdb, uint8_t cdb_size, void* data, uint32_t data_size,
                    bool data_in) {
	if (cdb == NULL || cdb_size == 0u || cdb_size > 16u || (data_size != 0u && data == NULL)) return false;
	uint32_t tag = ++device->next_tag;
	if (tag == 0u) tag = ++device->next_tag;
	uint8_t cbw[USB_MSC_CBW_SIZE] = {0};
	put_le32(cbw, USB_MSC_CBW_SIGNATURE);
	put_le32(cbw + 4u, tag);
	put_le32(cbw + 8u, data_size);
	cbw[12] = data_in ? 0x80u : 0u;
	cbw[14] = cdb_size;
	memcpy(cbw + 15u, cdb, cdb_size);
	if (!transfer_all(device, device->bulk_out, cbw, sizeof(cbw))) return false;
	if (data_size != 0u && !transfer_all(device, data_in ? device->bulk_in : device->bulk_out, data, data_size))
		return false;
	uint8_t csw[USB_MSC_CSW_SIZE];
	if (!transfer_all(device, device->bulk_in, csw, sizeof(csw)) || get_le32(csw) != USB_MSC_CSW_SIGNATURE ||
	    get_le32(csw + 4u) != tag || get_le32(csw + 8u) != 0u || csw[12] != 0u)
		return false;
	return true;
}

static bool read_capacity(struct usb_msc_device* device) {
	uint8_t cdb[16]      = {0x25u};
	uint8_t response[32] = {0};
	if (!command(device, cdb, 10u, response, 8u, true)) return false;
	uint64_t last_block = get_be32(response);
	uint32_t block_size = get_be32(response + 4u);
	if (last_block == UINT32_MAX) {
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = 0x9eu;
		cdb[1] = 0x10u;
		put_be32(cdb + 10u, sizeof(response));
		if (!command(device, cdb, 16u, response, sizeof(response), true)) return false;
		last_block = get_be64(response);
		block_size = get_be32(response + 8u);
	}
	if (last_block == UINT64_MAX || block_size == 0u || block_size > USB_MSC_TRANSFER_LIMIT) return false;
	device->block_count = last_block + 1u;
	device->block_size  = block_size;
	uint32_t limit      = USB_MSC_TRANSFER_LIMIT / block_size;
	if (limit > UINT16_MAX) limit = UINT16_MAX;
	device->max_transfer_blocks = limit;
	return limit != 0u;
}

static bool ready(struct usb_msc_device* device) {
	uint8_t cdb[6] = {0u};
	for (unsigned attempt = 0u; attempt < 20u; attempt++) {
		if (command(device, cdb, sizeof(cdb), NULL, 0u, true)) return true;
		if (sleep_ms(50u) != SYSCALL_STATUS_OK) return false;
	}
	return false;
}

bool usb_msc_initialize(cap_id_t device_cap, struct usb_msc_device* device) {
	if (device == NULL) return false;
	*device = (struct usb_msc_device){.transport_cap = CAP_ID_INVALID};
	if (device_resource_acquire(device_cap,
	                            USB_TRANSPORT_RESOURCE,
	                            sizeof(USB_TRANSPORT_RESOURCE) - 1u,
	                            USB_TRANSPORT_RIGHTS,
	                            &device->transport_cap) != SYSCALL_STATUS_OK)
		return false;
	struct usb_transport_header query = {.op = USB_TRANSPORT_OP_INFO};
	struct usb_transport_info   info;
	size_t                      response_size;
	if (cap_call(device->transport_cap, &query, sizeof(query), &info, sizeof(info), &response_size) !=
	        SYSCALL_STATUS_OK ||
	    response_size != sizeof(info) || info.class_code != USB_MSC_CLASS || info.subclass != USB_MSC_SCSI_SUBCLASS ||
	    info.programming_interface != USB_MSC_BULK_ONLY_PROTOCOL)
		goto failed;
	device->interface_number = info.interface_number;
	for (unsigned i = 0u; i < info.endpoint_count; i++) {
		if ((info.endpoints[i].attributes & 3u) != USB_ENDPOINT_BULK) continue;
		if ((info.endpoints[i].address & 0x80u) != 0u) device->bulk_in = info.endpoints[i].address;
		else device->bulk_out = info.endpoints[i].address;
	}
	if (device->bulk_in == 0u || device->bulk_out == 0u) goto failed;
	uint8_t inquiry_cdb[6] = {0x12u, 0u, 0u, 0u, 36u, 0u};
	uint8_t inquiry[36]    = {0};
	if (!command(device, inquiry_cdb, sizeof(inquiry_cdb), inquiry, sizeof(inquiry), true) ||
	    (inquiry[0] & 0x1fu) != 0u || !ready(device) || !read_capacity(device))
		goto failed;
	device->removable = (inquiry[1] & 0x80u) != 0u;
	void* probe       = malloc(device->block_size);
	if (probe == NULL || !usb_msc_read(device, 0u, 1u, probe)) {
		free(probe);
		goto failed;
	}
	free(probe);
	return true;

failed:
	usb_msc_deinitialize(device);
	return false;
}

static bool blocks(struct usb_msc_device* device, uint64_t block, uint32_t count, void* data, bool write) {
	if (device == NULL || data == NULL || count == 0u || count > device->max_transfer_blocks ||
	    block >= device->block_count || count > device->block_count - block || count > UINT32_MAX / device->block_size)
		return false;
	uint32_t size    = count * device->block_size;
	uint8_t  cdb[16] = {0};
	uint8_t  cdb_size;
	if (block <= UINT32_MAX && count <= UINT16_MAX && count - 1u <= UINT32_MAX - block) {
		cdb[0] = write ? 0x2au : 0x28u;
		put_be32(cdb + 2u, (uint32_t)block);
		put_be16(cdb + 7u, (uint16_t)count);
		cdb_size = 10u;
	}
	else {
		cdb[0] = write ? 0x8au : 0x88u;
		put_be64(cdb + 2u, block);
		put_be32(cdb + 10u, count);
		cdb_size = 16u;
	}
	return command(device, cdb, cdb_size, data, size, !write);
}

bool usb_msc_read(struct usb_msc_device* device, uint64_t block, uint32_t count, void* data) {
	return blocks(device, block, count, data, false);
}

bool usb_msc_write(struct usb_msc_device* device, uint64_t block, uint32_t count, const void* data) {
	return blocks(device, block, count, (void*)data, true);
}

bool usb_msc_flush(struct usb_msc_device* device) {
	uint8_t cdb[10] = {0x35u};
	return command(device, cdb, sizeof(cdb), NULL, 0u, true);
}

void usb_msc_deinitialize(struct usb_msc_device* device) {
	if (device == NULL) return;
	if (device->transport_cap != CAP_ID_INVALID) (void)cap_drop(device->transport_cap);
	*device = (struct usb_msc_device){.transport_cap = CAP_ID_INVALID};
}
