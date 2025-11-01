#pragma once

#include <limine.h>
#include <stdbool.h>

bool supports_limine_base_revision(void);

extern volatile struct limine_framebuffer_request fb_req;
extern volatile struct limine_memmap_request	  memmap_req;
