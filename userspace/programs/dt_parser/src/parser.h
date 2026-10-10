#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

/* Translate every enabled, compatible Device Tree node into one root device. */
syscall_status_t dt_parser_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                 cap_id_t dma_cap, cap_id_t interrupts_cap, size_t* device_count);
