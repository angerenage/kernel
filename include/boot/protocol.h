#pragma once

#include <boot/info.h>
#include <core/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Initialize the selected boot backend and publish its immutable boot_info. */
bool boot_init(void);

/* Active CPU startup services; these are deliberately not members of boot_info. */
typedef void (*boot_cpu_entry_t)(size_t cpu_index, void* arg);

bool boot_cpu_mp_supported(void);
bool boot_cpu_topology(struct cpu_init_info* init_info, size_t max_count, uintptr_t boot_stack_base,
                       uintptr_t boot_stack_top, size_t* out_cpu_count, size_t* out_bsp_index);
bool boot_cpu_start(size_t cpu_index, boot_cpu_entry_t entry, void* arg);
