#pragma once

#include <base/cap.h>
#include <stdbool.h>

struct usb_msc_device;

bool usb_msc_block_service_run(cap_id_t device_cap, struct usb_msc_device* device);
