#include <kernel/requests.h>

#define LIMINE_TARGET_BASE_REVISION 6

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(LIMINE_TARGET_BASE_REVISION);

/* Framebuffer request */
__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request fb_req = {
	.id       = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0,
};

/* Memory map request */
__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_req = {
	.id       = LIMINE_MEMMAP_REQUEST,
	.revision = 0,
};

/* HHDM request */
__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_req = {
	.id       = LIMINE_HHDM_REQUEST,
	.revision = 0,
};

/* Executable command line */
__attribute__((used, section(".limine_requests")))
volatile struct limine_executable_cmdline_request cmdline_req = {
	.id       = LIMINE_EXECUTABLE_CMDLINE_REQUEST,
	.revision = 0,
};

/* RSDP request */
__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_req = {
	.id       = LIMINE_RSDP_REQUEST,
	.revision = 0,
};

/* Executable address */
__attribute__((used, section(".limine_requests")))
volatile struct limine_kernel_address_request exec_addr_req = {
	.id       = LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision = 0,
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

bool supports_limine_base_revision(void) {
	return LIMINE_BASE_REVISION_SUPPORTED != 0;
}
