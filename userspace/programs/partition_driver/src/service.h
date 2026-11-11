#pragma once

#include <stdbool.h>

#include <base/cap.h>

struct block_device;
struct partition_table;

/* Publish block:partition children backed directly by bounded provider capabilities. */
bool partition_service_run(cap_id_t device_cap, struct block_device* parent, const struct partition_table* partitions);
