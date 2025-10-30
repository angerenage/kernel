#pragma once

#include <stdbool.h>
#include <limine.h>

bool supports_limine_base_revision(void);

extern volatile struct limine_framebuffer_request fb_req;
extern volatile struct limine_memmap_request memmap_req;
