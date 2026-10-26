#pragma once

#include <base/cap.h>
#include <stddef.h>

#include "device.h"

#define DM_DRIVER_CATALOG_PATH "/boot/boot-drivers.yaml"

/* Result of loading and validating one driver catalog. */
enum dm_driver_catalog_status {
	DM_DRIVER_CATALOG_OK = 0u,
	DM_DRIVER_CATALOG_IO_ERROR,
	DM_DRIVER_CATALOG_YAML_ERROR,
	DM_DRIVER_CATALOG_INVALID,
	DM_DRIVER_CATALOG_NO_MEMORY,
};

/* Root authorities that a catalog entry may explicitly request. */
enum dm_driver_capability {
	DM_DRIVER_CAPABILITY_MEMORY_ALLOCATOR = 1u << 0u,
	DM_DRIVER_CAPABILITY_DMA              = 1u << 1u,
	DM_DRIVER_CAPABILITY_INTERRUPTS       = 1u << 2u,
	DM_DRIVER_CAPABILITY_IO_PORTS         = 1u << 3u,
};

/* One immutable driver declaration from the catalog. */
struct dm_driver {
	struct dm_string  name;
	struct dm_string  executable;
	struct dm_string* compatible_ids;
	size_t            compatible_count;
	uint32_t          root_capabilities;
	cap_id_t          executable_cap;
	struct dm_driver* next;
};

/* Complete validated catalog retained by the device manager. */
struct dm_driver_catalog {
	struct dm_driver* drivers;
	struct dm_driver* drivers_tail;
	size_t            driver_count;
};

/* Initialize an empty driver catalog. */
void dm_driver_catalog_init(struct dm_driver_catalog* catalog);

/* Release all declarations and strings retained by a driver catalog. */
void dm_driver_catalog_deinit(struct dm_driver_catalog* catalog);

/* Load one strict version-one YAML catalog without exposing partial state. */
enum dm_driver_catalog_status dm_driver_catalog_load(struct dm_driver_catalog* catalog, const char* path);

/* Find the first catalog-ordered driver claiming an exact compatible ID. */
const struct dm_driver* dm_driver_catalog_find_compatible(const struct dm_driver_catalog* catalog,
                                                          const void* compatible_id, size_t compatible_id_size);

/* Return a stable diagnostic string for one catalog result. */
const char* dm_driver_catalog_status_string(enum dm_driver_catalog_status status);
