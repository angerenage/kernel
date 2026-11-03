#pragma once

#include <base/cap.h>
#include <stdint.h>

#define USB_TRANSPORT_RESOURCE "transport"
#define USB_COMPATIBLE_DEVICE "usb:device"
#define USB_TRANSPORT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE))
#define USB_TRANSPORT_MAX_TRANSFER 4096u
#define USB_TRANSPORT_MAX_ENDPOINTS 30u

enum usb_transport_op {
	USB_TRANSPORT_OP_INFO = 0u,
	USB_TRANSPORT_OP_TRANSFER,
};

struct usb_transport_header {
	enum usb_transport_op op;
};

struct usb_transport_endpoint {
	uint8_t  address;
	uint8_t  attributes;
	uint8_t  interval;
	uint8_t  reserved;
	uint16_t max_packet_size;
	uint16_t reserved2;
};

struct usb_transport_info {
	uint16_t                      vendor_id;
	uint16_t                      product_id;
	uint8_t                       configuration;
	uint8_t                       endpoint_count;
	uint8_t                       reserved[2];
	struct usb_transport_endpoint endpoints[USB_TRANSPORT_MAX_ENDPOINTS];
};

/* Request a transfer on an endpoint belonging to the child device. */
struct usb_transport_transfer_request {
	struct usb_transport_header header;
	uint8_t                     endpoint;
	uint8_t                     reserved[3];
	uint32_t                    length;
	uint8_t                     data[]; /* OUT payload; absent for IN. */
};

/* Return the transferred length and the IN payload, if any. */
struct usb_transport_transfer_response {
	uint32_t transferred;
	uint8_t  data[];
};
