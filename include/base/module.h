#pragma once

#include <stddef.h>
#include <stdint.h>

/* Lightweight descriptor for a boot module loaded alongside the kernel image. */
struct module_info {
	size_t size;
	char   name[64];
};
