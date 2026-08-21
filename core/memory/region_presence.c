#include <core/mm.h>
#include <core/pmm.h>
#include <core/region_presence.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define REGION_PRESENCE_HEADER_SIZE (sizeof(struct region_presence_chunk*) + sizeof(uintptr_t) + sizeof(size_t))
#define REGION_PRESENCE_WORD_COUNT ((PMM_PAGE_SIZE - REGION_PRESENCE_HEADER_SIZE) / sizeof(uint64_t))
#define REGION_PRESENCE_PAGES_PER_CHUNK (REGION_PRESENCE_WORD_COUNT * 64u)
#define REGION_PRESENCE_INLINE_PAGES 64u

struct region_presence_chunk {
	struct region_presence_chunk* next;
	uintptr_t                     phys;
	size_t                        first_page;
	uint64_t                      bits[REGION_PRESENCE_WORD_COUNT];
};

_Static_assert(sizeof(struct region_presence_chunk) <= PMM_PAGE_SIZE, "presence chunk must fit in one page");

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static size_t chunk_first_page(size_t page_index) {
	return REGION_PRESENCE_INLINE_PAGES +
	       ((page_index - REGION_PRESENCE_INLINE_PAGES) / REGION_PRESENCE_PAGES_PER_CHUNK) *
	           REGION_PRESENCE_PAGES_PER_CHUNK;
}

static struct region_presence_chunk* find_chunk(const struct region_presence* presence, size_t first_page) {
	struct region_presence_chunk* chunk;

	if (!presence) return NULL;
	for (chunk = presence->chunks; chunk != NULL; chunk = chunk->next) {
		if (chunk->first_page == first_page) return chunk;
		if (chunk->first_page > first_page) break;
	}
	return NULL;
}

void region_presence_init(struct region_presence* presence) {
	if (presence) *presence = (struct region_presence){0};
}

void region_presence_release(struct region_presence* presence) {
	struct region_presence_chunk* chunk;

	if (!presence) return;
	chunk = presence->chunks;
	while (chunk != NULL) {
		struct region_presence_chunk* next = chunk->next;

		(void)pmm_free_pages(chunk->phys, 1u);
		chunk = next;
	}
	region_presence_init(presence);
}

bool region_presence_prepare(struct region_presence* presence, size_t page_index) {
	struct region_presence_chunk** link;
	struct region_presence_chunk*  chunk;
	uintptr_t                      phys = 0;
	size_t                         first_page;

	if (!presence) return false;
	if (page_index < REGION_PRESENCE_INLINE_PAGES) return true;
	first_page = chunk_first_page(page_index);
	link       = &presence->chunks;
	while (*link != NULL && (*link)->first_page < first_page) link = &(*link)->next;
	if (*link != NULL && (*link)->first_page == first_page) return true;
	if (!pmm_alloc_pages(1u, &phys)) return false;
	chunk = hhdm_phys_to_virt(phys);
	memset(chunk, 0, PMM_PAGE_SIZE);
	chunk->next       = *link;
	chunk->phys       = phys;
	chunk->first_page = first_page;
	*link             = chunk;
	return true;
}

void region_presence_discard_empty(struct region_presence* presence, size_t page_index) {
	struct region_presence_chunk** link;
	struct region_presence_chunk*  chunk;
	bool                           empty = true;
	size_t                         first_page;

	if (!presence || page_index < REGION_PRESENCE_INLINE_PAGES) return;
	first_page = chunk_first_page(page_index);
	link       = &presence->chunks;
	while (*link != NULL && (*link)->first_page < first_page) link = &(*link)->next;
	chunk = *link;
	if (!chunk || chunk->first_page != first_page) return;
	for (size_t word = 0; word < REGION_PRESENCE_WORD_COUNT; word++) {
		if (chunk->bits[word] != 0) {
			empty = false;
			break;
		}
	}
	if (!empty) return;
	*link = chunk->next;
	(void)pmm_free_pages(chunk->phys, 1u);
}

bool region_presence_is_mapped(const struct region_presence* presence, size_t page_index) {
	struct region_presence_chunk* chunk;
	size_t                        offset;

	if (!presence) return false;
	if (page_index < REGION_PRESENCE_INLINE_PAGES) {
		return (presence->inline_bits & ((uint64_t)1u << page_index)) != 0;
	}
	chunk = find_chunk(presence, chunk_first_page(page_index));
	if (!chunk) return false;
	offset = page_index - chunk->first_page;
	return (chunk->bits[offset / 64u] & ((uint64_t)1u << (offset % 64u))) != 0;
}

void region_presence_mark_mapped(struct region_presence* presence, size_t page_index) {
	struct region_presence_chunk* chunk;
	size_t                        offset;
	uint64_t                      mask;

	if (!presence) return;
	if (page_index < REGION_PRESENCE_INLINE_PAGES) {
		mask = (uint64_t)1u << page_index;
		if ((presence->inline_bits & mask) != 0) return;
		presence->inline_bits |= mask;
		presence->mapped_count++;
		return;
	}
	chunk = find_chunk(presence, chunk_first_page(page_index));
	if (!chunk) return;
	offset = page_index - chunk->first_page;
	mask   = (uint64_t)1u << (offset % 64u);
	if ((chunk->bits[offset / 64u] & mask) != 0) return;
	chunk->bits[offset / 64u] |= mask;
	presence->mapped_count++;
}

void region_presence_clear_mapped(struct region_presence* presence, size_t page_index) {
	struct region_presence_chunk* chunk;
	size_t                        offset;
	uint64_t                      mask;

	if (!presence) return;
	if (page_index < REGION_PRESENCE_INLINE_PAGES) {
		mask = (uint64_t)1u << page_index;
		if ((presence->inline_bits & mask) == 0) return;
		presence->inline_bits &= ~mask;
		presence->mapped_count--;
		return;
	}
	chunk = find_chunk(presence, chunk_first_page(page_index));
	if (!chunk) return;
	offset = page_index - chunk->first_page;
	mask   = (uint64_t)1u << (offset % 64u);
	if ((chunk->bits[offset / 64u] & mask) == 0) return;
	chunk->bits[offset / 64u] &= ~mask;
	presence->mapped_count--;
	region_presence_discard_empty(presence, page_index);
}

size_t region_presence_mapped_count(const struct region_presence* presence) {
	return presence != NULL ? presence->mapped_count : 0u;
}
