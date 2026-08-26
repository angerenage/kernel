#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct kernel_boot_module      mock_boot_modules[16];
static size_t                         mock_boot_module_count;
static struct kernel_boot_data        mock_rsdp;
static bool                           mock_rsdp_valid;
static struct kernel_boot_data        mock_dtb;
static bool                           mock_dtb_valid;
static struct kernel_boot_framebuffer mock_framebuffer;
static bool                           mock_framebuffer_valid;

void kernel_boot_mock_set_modules(const struct kernel_boot_module* modules, size_t count) {
	if (count > 16) count = 16;
	mock_boot_module_count = count;
	for (size_t i = 0; i < count; i++) {
		mock_boot_modules[i] = modules[i];
	}
}

void kernel_boot_mock_reset(void) {
	mock_boot_module_count = 0;
	mock_rsdp              = (struct kernel_boot_data){0};
	mock_rsdp_valid        = false;
	mock_dtb               = (struct kernel_boot_data){0};
	mock_dtb_valid         = false;
	mock_framebuffer       = (struct kernel_boot_framebuffer){0};
	mock_framebuffer_valid = false;
}

void kernel_boot_mock_set_rsdp(const void* address, size_t size) {
	mock_rsdp       = (struct kernel_boot_data){.address = address, .size = size};
	mock_rsdp_valid = address != NULL && size != 0u;
}

void kernel_boot_mock_set_dtb(const void* address, size_t size) {
	mock_dtb       = (struct kernel_boot_data){.address = address, .size = size};
	mock_dtb_valid = address != NULL && size != 0u;
}

void kernel_boot_mock_set_framebuffer(const struct kernel_boot_framebuffer* framebuffer) {
	mock_framebuffer_valid = framebuffer != NULL;
	mock_framebuffer       = framebuffer != NULL ? *framebuffer : (struct kernel_boot_framebuffer){0};
}

bool kernel_boot_rsdp_get(struct kernel_boot_data* out) {
	if (out == NULL || !mock_rsdp_valid) return false;
	*out = mock_rsdp;
	return true;
}

bool kernel_boot_dtb_get(struct kernel_boot_data* out) {
	if (out == NULL || !mock_dtb_valid) return false;
	*out = mock_dtb;
	return true;
}

bool kernel_boot_framebuffer_get(struct kernel_boot_framebuffer* out) {
	if (out == NULL || !mock_framebuffer_valid) return false;
	*out = mock_framebuffer;
	return true;
}

size_t kernel_boot_module_count(void) {
	return mock_boot_module_count;
}

const struct kernel_boot_module* kernel_boot_module_at(size_t index) {
	if (index >= mock_boot_module_count) return NULL;
	return &mock_boot_modules[index];
}

const struct kernel_boot_module* kernel_boot_module_find(const char* name) {
	for (size_t i = 0; i < mock_boot_module_count; i++) {
		if (mock_boot_modules[i].name != NULL && strcmp(mock_boot_modules[i].name, name) == 0) {
			return &mock_boot_modules[i];
		}
		if (mock_boot_modules[i].path != NULL && strcmp(mock_boot_modules[i].path, name) == 0) {
			return &mock_boot_modules[i];
		}
	}
	return NULL;
}
