#pragma once

#include <base/cap.h>
#include <stdbool.h>

struct block_device;
struct partition_table;

/* Publish bounded block:partition children and serve their translated requests. */
bool partition_service_run(cap_id_t device_cap, const struct block_device* parent,
                           const struct partition_table* partitions);
