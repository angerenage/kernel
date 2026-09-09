#pragma once

#include <base/memory.h>
#include <stddef.h>

/* Hosted tests use the mock AddressSpace translation granule. */
#define TEST_MAPPING_GRANULE ((size_t)4096u)
