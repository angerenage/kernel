#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <base/cap.h>

struct usb_msc_device {
	cap_id_t transport_cap;
	uint32_t next_tag;
	uint64_t block_count;
	uint32_t block_size;
	uint32_t max_transfer_blocks;
	uint8_t bulk_in;
	uint8_t bulk_out;
	uint8_t interface_number;
	bool removable;
};

bool usb_msc_initialize(cap_id_t device_cap, struct usb_msc_device* device);
void usb_msc_deinitialize(struct usb_msc_device* device);

bool usb_msc_read(struct usb_msc_device* device, uint64_t block, uint32_t count, void* data);
bool usb_msc_write(struct usb_msc_device* device, uint64_t block, uint32_t count, const void* data);
bool usb_msc_flush(struct usb_msc_device* device);
