#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "limine.h"

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootloader_request = {
	.id = LIMINE_BOOTLOADER_INFO_REQUEST,
	.revision = 0,
	.response = NULL,
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

static __attribute__((noreturn)) void hcf(void) {
	__asm__ volatile("cli");
	for (;;) {
		__asm__ volatile("hlt");
	}
}

__attribute__((noreturn))
void kernel_main(void) {
	if (!LIMINE_BASE_REVISION_SUPPORTED) {
		hcf();
	}

	if (bootloader_request.response == NULL) {
		hcf();
	}

	for (;;) {
		__asm__ volatile("hlt");
	}
}
