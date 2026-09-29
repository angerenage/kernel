#pragma once

#include <stdbool.h>

#include "limine.h"

bool limine_protocol_supported(void);

extern volatile struct limine_framebuffer_request        fb_req;
extern volatile struct limine_mp_request                 mp_req;
extern volatile struct limine_memmap_request             memmap_req;
extern volatile struct limine_hhdm_request               hhdm_req;
extern volatile struct limine_rsdp_request               rsdp_req;
extern volatile struct limine_dtb_request                dtb_req;
extern volatile struct limine_executable_cmdline_request cmdline_req;
extern volatile struct limine_executable_address_request exec_addr_req;
extern volatile struct limine_module_request             module_req;
