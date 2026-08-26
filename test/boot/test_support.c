#include "test_support.h"

#include <hal/cpu.h>
#include <stdlib.h>
#include <string.h>

#undef PLATFORM_PC_AARCH64
#undef PLATFORM_PC_RISCV64
#undef PLATFORM_PC_LOONGARCH64
#ifndef PLATFORM_PC_X86_64
#define PLATFORM_PC_X86_64 1
#endif

#include "../../kernel/src/boot/limine_requests.h"

volatile struct limine_framebuffer_request fb_req;
volatile struct LIMINE_MP(request) mp_req;
volatile struct limine_memmap_request             memmap_req;
volatile struct limine_hhdm_request               hhdm_req;
volatile struct limine_rsdp_request               rsdp_req;
volatile struct limine_dtb_request                dtb_req;
volatile struct limine_executable_cmdline_request cmdline_req;
volatile struct limine_kernel_address_request     exec_addr_req;
volatile struct limine_module_request             module_req;

static bool     protocol_supported;
static uint64_t boot_arch_id = 0x2au;

bool kernel_limine_protocol_supported(void) {
	return protocol_supported;
}

uint64_t hal_cpu_boot_arch_id(void) {
	return boot_arch_id;
}

__attribute__((noreturn))
void hal_cpu_park(void) {
	abort();
}

#include "../../kernel/src/boot/boot.c"

static struct limine_memmap_entry            usable_entry;
static struct limine_memmap_entry*           memmap_entries[1];
static struct limine_memmap_response         memmap_response;
static struct limine_hhdm_response           hhdm_response;
static struct limine_kernel_address_response exec_response;
static struct limine_module_response         module_response;
static struct LIMINE_MP(response) mp_response;

void boot_test_reset(void) {
	memset(boot_memmap, 0, sizeof(boot_memmap));
	boot_memmap_count = 0u;
	memset(&boot_framebuffer, 0, sizeof(boot_framebuffer));
	boot_framebuffer_valid = false;
	memset(&boot_address_space, 0, sizeof(boot_address_space));
	boot_address_space_valid = false;
	boot_cmdline             = NULL;
	memset(boot_modules, 0, sizeof(boot_modules));
	boot_module_count = 0u;
	boot_rsdp_address = 0u;
	boot_rsdp_size    = 0u;
	boot_rsdp_valid   = false;
	boot_dtb_address  = NULL;
	boot_dtb_size     = 0u;
	boot_dtb_valid    = false;
	memset(boot_cpu_launch, 0, sizeof(boot_cpu_launch));
	memset(boot_cpu_private, 0, sizeof(boot_cpu_private));
	boot_cpu_count   = 0u;
	boot_initialized = false;

	memset((void*)&fb_req, 0, sizeof(fb_req));
	memset((void*)&mp_req, 0, sizeof(mp_req));
	memset((void*)&memmap_req, 0, sizeof(memmap_req));
	memset((void*)&hhdm_req, 0, sizeof(hhdm_req));
	memset((void*)&rsdp_req, 0, sizeof(rsdp_req));
	memset((void*)&dtb_req, 0, sizeof(dtb_req));
	memset((void*)&cmdline_req, 0, sizeof(cmdline_req));
	memset((void*)&exec_addr_req, 0, sizeof(exec_addr_req));
	memset((void*)&module_req, 0, sizeof(module_req));

	protocol_supported = false;
	boot_arch_id       = 0x2au;
}

void boot_test_configure_valid_base(void) {
	boot_test_reset();
	protocol_supported = true;

	usable_entry = (struct limine_memmap_entry){
		.base   = 0x100000u,
		.length = 0x400000u,
		.type   = LIMINE_MEMMAP_USABLE,
	};
	memmap_entries[0] = &usable_entry;
	memmap_response   = (struct limine_memmap_response){
		  .entry_count = 1u,
		  .entries     = memmap_entries,
    };
	hhdm_response = (struct limine_hhdm_response){.offset = 0xffff800000000000ull};
	exec_response = (struct limine_kernel_address_response){
		.physical_base = 0x200000u,
		.virtual_base  = 0xffffffff80000000ull,
	};

	memmap_req.response    = &memmap_response;
	hhdm_req.response      = &hhdm_response;
	exec_addr_req.response = &exec_response;
}

void boot_test_configure_module_count(uint64_t module_count) {
	module_response = (struct limine_module_response){
		.module_count = module_count,
		.modules      = NULL,
	};
	module_req.response = &module_response;
}

void boot_test_configure_mp(struct LIMINE_MP(info) * *cpus, uint64_t cpu_count, uint64_t bsp_arch_id) {
	mp_response = (struct LIMINE_MP(response)){
		.bsp_lapic_id = (uint32_t)bsp_arch_id,
		.cpu_count    = cpu_count,
		.cpus         = cpus,
	};
	mp_req.response = &mp_response;
}
