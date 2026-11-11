#pragma once

#include <stdbool.h>

#include <base/cap.h>

struct usb_msc_device;

bool usb_msc_block_service_run(cap_id_t device_cap, struct usb_msc_device* device);
