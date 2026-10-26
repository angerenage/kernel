#pragma once

#include <base/cap.h>
#include <stdint.h>

/* Registry identity and version of the device-manager control service. */
#define DEVICE_MANAGER_NAMESPACE "system"
#define DEVICE_MANAGER_PROTOCOL_NAME "device-manager-control"
#define DEVICE_MANAGER_SERVICE_NAME "default"
#define DEVICE_MANAGER_PROTOCOL_VERSION_MAJOR 1u
#define DEVICE_MANAGER_PROTOCOL_VERSION_MINOR 0u

/* Ordinary clients may inspect the catalog; init may request CAP_WRITE to replace it. */
#define DEVICE_MANAGER_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))
#define DEVICE_MANAGER_CONTROL_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE))

/* Operations accepted by the device-manager control service. */
enum device_manager_op {
	DEVICE_MANAGER_OP_CATALOG_INFO = 0u,
	DEVICE_MANAGER_OP_UPDATE_CATALOG,
};

/* Common operation header for device-manager control requests. */
struct device_manager_request_header {
	enum device_manager_op op;
};

/* Request the active catalog generation and declaration count. */
struct device_manager_catalog_info_request {
	struct device_manager_request_header header;
};

/* Metadata identifying one successfully activated catalog. */
struct device_manager_catalog_info {
	uint64_t generation;
	uint64_t driver_count;
};

/* Detailed outcome of parsing and activating a replacement catalog. */
enum device_manager_catalog_update_status {
	DEVICE_MANAGER_CATALOG_UPDATE_OK = 0u,
	DEVICE_MANAGER_CATALOG_UPDATE_IO_ERROR,
	DEVICE_MANAGER_CATALOG_UPDATE_YAML_ERROR,
	DEVICE_MANAGER_CATALOG_UPDATE_INVALID,
	DEVICE_MANAGER_CATALOG_UPDATE_NO_MEMORY,
};

/* Replace the catalog from the absolute VFS path following this header. */
struct device_manager_update_catalog_request {
	struct device_manager_request_header header;
	uint32_t                             path_size;
	uint32_t                             reserved;
	uint8_t                              path[];
};

/* Catalog result and metadata; metadata changes only after successful activation. */
struct device_manager_update_catalog_response {
	enum device_manager_catalog_update_status status;
	uint32_t                                  reserved;
	struct device_manager_catalog_info        catalog;
};
