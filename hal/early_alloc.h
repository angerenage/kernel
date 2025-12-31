#pragma once

#include <limine.h>
#include <stddef.h>

#include "mm.h"

bool early_init(const struct limine_memmap_response* memmap_resp, uintptr_t direct_map_offset);
