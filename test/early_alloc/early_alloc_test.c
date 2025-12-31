#include <core/mm.h>
#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KiB(x) ((size_t)(x) * 1024u)

bool is_aligned_uintptr(uintptr_t p, size_t align) {
	return (align == 0) ? true : ((p & (align - 1)) == 0);
}

void init_early_allocator(uint8_t* arena, size_t arena_size) {
	struct limine_memmap_entry entry = {
		.base   = (uint64_t)(uintptr_t)arena,
		.length = (uint64_t)arena_size,
		.type   = LIMINE_MEMMAP_USABLE,
	};

	struct limine_memmap_entry* entries[] = {&entry};

	struct limine_memmap_response resp = {
		.entry_count = 1,
		.entries     = entries,
	};

	bool ok = early_init(&resp, 0);
	cr_assert(ok, "early_init failed");
}
