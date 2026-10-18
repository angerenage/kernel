#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct terminal_glyph_cache;

/* One immutable, normalized glyph bitmap retained by the terminal cache. */
struct terminal_glyph {
	uint32_t       codepoint;
	int64_t        left;
	int64_t        top;
	size_t         width;
	size_t         height;
	const uint8_t* alpha;
};

/* Create a bounded glyph cache backed by one configured FreeType face. */
struct terminal_glyph_cache* terminal_glyph_cache_create(FT_Face face);

/* Release every normalized bitmap retained by a glyph cache. */
void terminal_glyph_cache_destroy(struct terminal_glyph_cache* cache);

/* Find or rasterize one Unicode codepoint and return its normalized bitmap. */
bool terminal_glyph_cache_get(struct terminal_glyph_cache* cache, uint32_t codepoint,
                              const struct terminal_glyph** out_glyph);
