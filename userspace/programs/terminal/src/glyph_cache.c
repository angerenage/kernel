#include "glyph_cache.h"

#include <base/math.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define TERMINAL_GLYPH_CACHE_SET_COUNT 128u
#define TERMINAL_GLYPH_CACHE_WAY_COUNT 4u

struct glyph_cache_entry {
	struct terminal_glyph glyph;
	uint8_t*              alpha;
	uint64_t              last_used;
	bool                  valid;
};

struct terminal_glyph_cache {
	FT_Face                  face;
	uint64_t                 clock;
	struct glyph_cache_entry entries[TERMINAL_GLYPH_CACHE_SET_COUNT][TERMINAL_GLYPH_CACHE_WAY_COUNT];
};

/* Convert one FreeType bitmap sample into canonical eight-bit coverage. */
static bool bitmap_alpha(const FT_Bitmap* bitmap, const uint8_t* row, size_t column, uint8_t* alpha) {
	if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
		if (bitmap->num_grays <= 1u) return false;
		*alpha = (uint8_t)(((uint32_t)row[column] * 255u) / (bitmap->num_grays - 1u));
		return true;
	}
	if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
		*alpha = (row[column / 8u] & (uint8_t)(0x80u >> (column % 8u))) != 0u ? 255u : 0u;
		return true;
	}
	return false;
}

/* Select the cache set for one Unicode scalar value. */
static size_t glyph_set_index(uint32_t codepoint) {
	uint32_t hash = codepoint;
	hash ^= hash >> 16u;
	hash *= 0x7feb352du;
	hash ^= hash >> 15u;
	return hash & (TERMINAL_GLYPH_CACHE_SET_COUNT - 1u);
}

/* Advance the replacement clock while preserving zero as the unused stamp. */
static uint64_t next_stamp(struct terminal_glyph_cache* cache) {
	cache->clock++;
	if (cache->clock != 0u) return cache->clock;
	for (size_t set = 0u; set < TERMINAL_GLYPH_CACHE_SET_COUNT; set++)
		for (size_t way = 0u; way < TERMINAL_GLYPH_CACHE_WAY_COUNT; way++)
			cache->entries[set][way].last_used = cache->entries[set][way].valid ? 1u : 0u;
	cache->clock = 2u;
	return cache->clock;
}

/* Rasterize one codepoint and normalize its bitmap before replacing a cache entry. */
static bool load_entry(struct terminal_glyph_cache* cache, struct glyph_cache_entry* entry, uint32_t codepoint,
                       uint64_t stamp) {
	struct glyph_cache_entry loaded = {0};
	FT_GlyphSlot             slot;
	const FT_Bitmap*         bitmap;
	size_t                   stride;
	size_t                   minimum_stride;
	size_t                   alpha_size;

	if (FT_Load_Char(cache->face, codepoint, FT_LOAD_RENDER) != FT_Err_Ok) return false;
	slot   = cache->face->glyph;
	bitmap = &slot->bitmap;
	if (bitmap->pitch == INT_MIN) return false;
	loaded.glyph = (struct terminal_glyph){
		.codepoint = codepoint,
		.left      = slot->bitmap_left,
		.top       = slot->bitmap_top,
		.width     = bitmap->width,
		.height    = bitmap->rows,
	};
	if (bitmap->width != 0u && bitmap->rows != 0u) {
		if (bitmap->buffer == NULL || mul_overflow_size(bitmap->width, bitmap->rows, &alpha_size)) return false;
		stride = (size_t)(bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch);
		if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) minimum_stride = bitmap->width;
		else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) minimum_stride = (bitmap->width + 7u) / 8u;
		else return false;
		if (stride < minimum_stride || (loaded.alpha = malloc(alpha_size)) == NULL) return false;
		for (size_t row = 0u; row < bitmap->rows; row++) {
			size_t source_row = bitmap->pitch < 0 ? bitmap->rows - row - 1u : row;
			size_t source_offset;
			if (mul_overflow_size(source_row, stride, &source_offset)) {
				free(loaded.alpha);
				return false;
			}
			for (size_t column = 0u; column < bitmap->width; column++) {
				if (!bitmap_alpha(
						bitmap, bitmap->buffer + source_offset, column, &loaded.alpha[row * bitmap->width + column])) {
					free(loaded.alpha);
					return false;
				}
			}
		}
		loaded.glyph.alpha = loaded.alpha;
	}
	loaded.last_used = stamp;
	loaded.valid     = true;
	free(entry->alpha);
	*entry = loaded;
	return true;
}

struct terminal_glyph_cache* terminal_glyph_cache_create(FT_Face face) {
	struct terminal_glyph_cache* cache;

	if (face == NULL) return NULL;
	cache = calloc(1u, sizeof(*cache));
	if (cache == NULL) return NULL;
	cache->face = face;
	return cache;
}

void terminal_glyph_cache_destroy(struct terminal_glyph_cache* cache) {
	if (cache == NULL) return;
	for (size_t set = 0u; set < TERMINAL_GLYPH_CACHE_SET_COUNT; set++)
		for (size_t way = 0u; way < TERMINAL_GLYPH_CACHE_WAY_COUNT; way++) free(cache->entries[set][way].alpha);
	free(cache);
}

bool terminal_glyph_cache_get(struct terminal_glyph_cache* cache, uint32_t codepoint,
                              const struct terminal_glyph** out_glyph) {
	struct glyph_cache_entry* entries;
	struct glyph_cache_entry* replacement;
	uint64_t                  stamp;

	if (cache == NULL || out_glyph == NULL || codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu))
		return false;
	*out_glyph  = NULL;
	stamp       = next_stamp(cache);
	entries     = cache->entries[glyph_set_index(codepoint)];
	replacement = &entries[0];
	for (size_t way = 0u; way < TERMINAL_GLYPH_CACHE_WAY_COUNT; way++) {
		if (entries[way].valid && entries[way].glyph.codepoint == codepoint) {
			entries[way].last_used = stamp;
			*out_glyph             = &entries[way].glyph;
			return true;
		}
		if (!entries[way].valid) replacement = &entries[way];
		else if (replacement->valid && entries[way].last_used < replacement->last_used) replacement = &entries[way];
	}
	if (!load_entry(cache, replacement, codepoint, stamp)) return false;
	*out_glyph = &replacement->glyph;
	return true;
}
