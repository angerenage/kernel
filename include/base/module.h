#pragma once

#include <stddef.h>
#include <stdint.h>

struct module_info {
	size_t size;
	char   name[64];
};
