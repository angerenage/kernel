#include <boot/info.h>
#include <boot/protocol.h>
#include <stdbool.h>
#include <string.h>

#include "limine/backend.h"

static const struct boot_info* published_info;

bool boot_init(void) {
	return published_info != NULL || limine_boot_init();
}

/* Used by a backend only after it has completely validated its local cache. */
bool boot_info_publish(const struct boot_info* info) {
	if (info == NULL || info->memory_map == NULL || info->memory_map_count == 0u || published_info != NULL)
		return false;
	published_info = info;
	return true;
}

const struct boot_info* boot_info_get(void) {
	return published_info;
}

bool boot_address_space_get(struct boot_address_space* out) {
	if (out == NULL || published_info == NULL) return false;
	*out = (struct boot_address_space){
		.direct_map_offset    = published_info->direct_map_offset,
		.kernel_physical_base = published_info->kernel_physical_base,
		.kernel_virtual_base  = published_info->kernel_virtual_base,
	};
	return true;
}

bool boot_framebuffer_get(struct boot_framebuffer* out) {
	if (out == NULL || published_info == NULL || !published_info->framebuffer_available) return false;
	*out = published_info->framebuffer;
	return true;
}

size_t boot_module_count(void) {
	return published_info == NULL ? 0u : published_info->module_count;
}

const struct boot_module* boot_module_get(size_t index) {
	return boot_module_at(published_info, index);
}

const struct boot_module* boot_module_lookup(const char* name) {
	return boot_module_find(published_info, name);
}

static const char* path_basename(const char* path) {
	const char* basename = path;
	if (path == NULL) return NULL;
	for (const char* cursor = path; *cursor != '\0'; cursor++)
		if (*cursor == '/' || *cursor == '\\') basename = cursor + 1;
	return basename;
}

const struct boot_module* boot_module_at(const struct boot_info* info, size_t index) {
	if (info == NULL || index >= info->module_count) return NULL;
	return &info->modules[index];
}

const struct boot_module* boot_module_find(const struct boot_info* info, const char* name) {
	if (info == NULL || name == NULL) return NULL;
	for (size_t index = 0u; index < info->module_count; index++) {
		const struct boot_module* module   = &info->modules[index];
		const char*               basename = path_basename(module->path);
		if ((module->name != NULL && strcmp(module->name, name) == 0) ||
		    (basename != NULL && strcmp(basename, name) == 0))
			return module;
	}
	return NULL;
}

const char* boot_cmdline(const struct boot_info* info) {
	return info == NULL ? NULL : info->command_line;
}

const char* boot_cmdline_current(void) {
	return boot_cmdline(published_info);
}
