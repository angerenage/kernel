#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct kernel_boot_module mock_boot_modules[16];
static size_t                    mock_boot_module_count;

void kernel_boot_mock_set_modules(const struct kernel_boot_module* modules, size_t count) {
	if (count > 16) count = 16;
	mock_boot_module_count = count;
	for (size_t i = 0; i < count; i++) {
		mock_boot_modules[i] = modules[i];
	}
}

void kernel_boot_mock_reset(void) {
	mock_boot_module_count = 0;
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
