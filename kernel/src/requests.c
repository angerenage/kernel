#include "requests.h"

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(4);

/* Framebuffer request */
__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request fb_req = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

/* Memory map request */
__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_req = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

bool supports_limine_base_revision(void) {
	return LIMINE_BASE_REVISION_SUPPORTED != 0;
}
