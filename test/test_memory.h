#pragma once

#include <stddef.h>

#include <base/memory.h>

/* Hosted tests use the mock AddressSpace translation granule. */
#define TEST_MAPPING_GRANULE ((size_t)4096u)
