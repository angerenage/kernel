#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct kernel_boot_module      mock_boot_modules[16];
static size_t                         mock_boot_module_count;
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
	mock_framebuffer       = (struct kernel_boot_framebuffer){0};
	mock_framebuffer_valid = false;
}

void kernel_boot_mock_set_framebuffer(const struct kernel_boot_framebuffer* framebuffer) {
	mock_framebuffer_valid = framebuffer != NULL;
	mock_framebuffer       = framebuffer != NULL ? *framebuffer : (struct kernel_boot_framebuffer){0};
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
