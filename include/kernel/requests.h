#pragma once

#include <limine.h>
#include <stdbool.h>

bool supports_limine_base_revision(void);

extern volatile struct limine_framebuffer_request    fb_req;
extern volatile struct limine_memmap_request         memmap_req;
extern volatile struct limine_hhdm_request           hhdm_req;
extern volatile struct limine_kernel_address_request exec_addr_req;
