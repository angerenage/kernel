#include <boot/info.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct boot_module      mock_boot_modules[16];
static size_t                  mock_boot_module_count;
static struct boot_framebuffer mock_framebuffer;
static bool                    mock_framebuffer_valid;
static struct boot_info        mock_boot_info;

void boot_mock_set_modules(const struct boot_module* modules, size_t count) {
	if (count > 16) count = 16;
	mock_boot_module_count = count;
	for (size_t i = 0; i < count; i++) {
		mock_boot_modules[i] = modules[i];
	}
	mock_boot_info.modules      = mock_boot_modules;
	mock_boot_info.module_count = count;
}

void boot_mock_reset(void) {
	mock_boot_module_count = 0;
	mock_framebuffer       = (struct boot_framebuffer){0};
	mock_framebuffer_valid = false;
	mock_boot_info         = (struct boot_info){.modules = mock_boot_modules};
}

void boot_mock_set_framebuffer(const struct boot_framebuffer* framebuffer) {
	mock_framebuffer_valid = framebuffer != NULL;
	mock_framebuffer       = framebuffer != NULL ? *framebuffer : (struct boot_framebuffer){0};
}

const struct boot_info* boot_info_get(void) {
	return &mock_boot_info;
}

bool boot_framebuffer_get(struct boot_framebuffer* out) {
	if (out == NULL || !mock_framebuffer_valid) return false;
	*out = mock_framebuffer;
	return true;
}

size_t boot_module_count(void) {
	return mock_boot_module_count;
}

const struct boot_module* boot_module_get(size_t index) {
	if (index >= mock_boot_module_count) return NULL;
	return &mock_boot_modules[index];
}

const struct boot_module* boot_module_lookup(const char* name) {
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
