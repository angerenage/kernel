#pragma once

#include <base/startup.h>
#include <stdbool.h>
#include <stddef.h>

extern uintptr_t runtime_heap_base;
extern size_t    runtime_heap_page_count;
extern size_t    runtime_heap_used_pages;
extern size_t    runtime_heap_page_size;
extern cap_id_t  runtime_heap_address_space_cap;

/* Initialize the runtime heap for the current process using the provided startup information. */
bool runtime_heap_init(const struct process_startup_info* startup);

/* Check if the runtime heap is configured. */
bool runtime_heap_is_configured(void);
