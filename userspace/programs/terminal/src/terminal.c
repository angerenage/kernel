#include "terminal.h"

#include <base/math.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glyph_cache.h"

#define FRAMEBUFFER_MEMORY_MODEL_RGB 1u
#define TERMINAL_FONT_PATH "/boot/CommitMono.otf"
#define TERMINAL_FONT_PIXEL_HEIGHT 18u
#define TERMINAL_TAB_WIDTH 4u
#define TERMINAL_CSI_PARAMETER_CAPACITY 16u
#define TERMINAL_SCROLLBACK_LINE_CAPACITY 4096u
#define TERMINAL_POINTER_MAX_WIDTH 11u
#define TERMINAL_POINTER_MAX_HEIGHT 20u
#define TERMINAL_POINTER_MAX_PIXEL_SIZE 8u

struct color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

struct terminal_theme {
	struct color background;
	struct color foreground;
	struct color selection_background;
	struct color selection_foreground;
	struct color cursor;
	struct color cursor_text;
	struct color palette[16];
};

struct pixel_format {
	size_t  bytes_per_pixel;
	uint8_t red_size;
	uint8_t red_shift;
	uint8_t green_size;
	uint8_t green_shift;
	uint8_t blue_size;
	uint8_t blue_shift;
};

struct drawing_surface {
	volatile uint8_t*   framebuffer;
	size_t              width;
	size_t              height;
	size_t              pitch;
	struct pixel_format format;
};

struct terminal_cell {
	uint32_t     codepoint;
	struct color foreground;
	struct color background;
	bool         underline;
};

struct terminal_overlay_cell {
	size_t               index;
	struct terminal_cell cell;
};

struct terminal_selection_state {
	struct terminal_selection_position anchor;
	struct terminal_selection_position focus;
	bool                               active;
};

struct terminal_pointer_shape {
	const char* pixels;
	size_t      stride;
	size_t      width;
	size_t      height;
	size_t      hotspot_x;
	size_t      hotspot_y;
};

enum escape_state {
	ESCAPE_STATE_TEXT,
	ESCAPE_STATE_ESCAPE,
	ESCAPE_STATE_CSI,
};

struct terminal {
	struct drawing_surface          surface;
	FT_Library                      library;
	FT_Face                         face;
	struct terminal_glyph_cache*    glyph_cache;
	uint8_t*                        font_data;
	size_t                          cell_width;
	size_t                          cell_height;
	size_t                          ascender;
	size_t                          columns;
	size_t                          rows;
	size_t                          column;
	size_t                          row;
	struct terminal_cell*           cells;
	size_t                          cell_count;
	struct terminal_cell*           history;
	size_t                          history_count;
	size_t                          history_head;
	uint64_t                        screen_origin;
	size_t                          viewport_offset;
	bool                            viewport_dirty;
	struct terminal_selection_state selection;
	uint8_t*                        clipboard;
	size_t                          clipboard_size;
	size_t                          saved_column;
	size_t                          saved_row;
	bool                            saved_cursor;
	bool                            cursor_visible;
	bool                            cursor_drawn;
	size_t                          cursor_drawn_row;
	size_t                          cursor_drawn_column;
	size_t                          pointer_x;
	size_t                          pointer_y;
	bool                            pointer_moved;
	bool                            pointer_drawn;
	size_t                          pointer_saved_x;
	size_t                          pointer_saved_y;
	size_t                          pointer_saved_width;
	size_t                          pointer_saved_height;
	uint8_t  pointer_saved[TERMINAL_POINTER_MAX_WIDTH * TERMINAL_POINTER_MAX_HEIGHT * TERMINAL_POINTER_MAX_PIXEL_SIZE];
	bool     pointer_primary_pressed;
	bool     pointer_selecting;
	uint64_t pointer_selection_source;
	struct terminal_selection_position pointer_selection_anchor;
	bool                               wrap_pending;
	bool                               locked;
	uint32_t*                          preedit_codepoints;
	size_t                             preedit_count;
	size_t                             preedit_caret;
	struct terminal_overlay_cell*      overlay;
	size_t                             overlay_count;
	bool                               foreground_default;
	bool                               background_default;
	bool                               bold;
	bool                               reverse;
	bool                               underline;
	uint8_t                            foreground;
	uint8_t                            background;
	uint32_t                           utf8_value;
	uint32_t                           utf8_minimum;
	uint8_t                            utf8_remaining;
	enum escape_state                  escape_state;
	uint16_t                           csi_parameters[TERMINAL_CSI_PARAMETER_CAPACITY];
	size_t                             csi_parameter_count;
	uint16_t                           csi_value;
	bool                               csi_has_value;
	bool                               csi_invalid;
	bool                               csi_private;
};

static const struct terminal_theme theme = {
	.background           = {0x1e, 0x1e, 0x1e},
	.foreground           = {0xcc, 0xcc, 0xcc},
	.selection_background = {0x3a, 0x3d, 0x41},
	.selection_foreground = {0xe0, 0xe0, 0xe0},
	.cursor               = {0xff, 0xff, 0xff},
	.cursor_text          = {0x00, 0x00, 0x00},
	.palette[0]           = {0x00, 0x00, 0x00}, // Black
	.palette[1]           = {0xd8, 0x2a, 0x2a}, // Red
	.palette[2]           = {0x22, 0xac, 0x3c}, // Green
	.palette[3]           = {0xe6, 0xe6, 0x03}, // Dark yellow
	.palette[4]           = {0x31, 0x45, 0x9c}, // Dark blue
	.palette[5]           = {0xd8, 0x22, 0x7f}, // Magenta
	.palette[6]           = {0x18, 0x99, 0xb9}, // Dark cyan
	.palette[7]           = {0x96, 0x96, 0x96}, // Light gray
	.palette[8]           = {0x50, 0x50, 0x50}, // Dark gray
	.palette[9]           = {0xf7, 0x49, 0x49}, // Light red
	.palette[10]          = {0xac, 0xe9, 0x71}, // Light green
	.palette[11]          = {0xf7, 0xf7, 0x61}, // Yellow
	.palette[12]          = {0x52, 0xa0, 0xf5}, // Blue
	.palette[13]          = {0xd6, 0x70, 0xd6}, // Light magenta
	.palette[14]          = {0x3d, 0xf8, 0xf7}, // Cyan
	.palette[15]          = {0xe5, 0xe5, 0xe5}, // White
};

static const char ibeam_pointer_pixels[][12] = {
	"BBBBBBBBBBB", "WWWWWWWWWWW", "....BWB....", "....BWB....", "....BWB....", "....BWB....", "....BWB....",
	"....BWB....", "...BBWBB...", "...BBWBB...", "...BBWBB...", "...BBWBB...", "....BWB....", "....BWB....",
	"....BWB....", "....BWB....", "....BWB....", "....BWB....", "WWWWWWWWWWW", "BBBBBBBBBBB",
};

static const struct terminal_pointer_shape ibeam_pointer_shape = {
	.pixels    = &ibeam_pointer_pixels[0][0],
	.stride    = sizeof(ibeam_pointer_pixels[0]),
	.width     = sizeof(ibeam_pointer_pixels[0]) - 1u,
	.height    = sizeof(ibeam_pointer_pixels) / sizeof(ibeam_pointer_pixels[0]),
	.hotspot_x = 5u,
	.hotspot_y = 10u,
};

_Static_assert(sizeof(ibeam_pointer_pixels[0]) - 1u <= TERMINAL_POINTER_MAX_WIDTH,
               "I-beam pointer is wider than its save-under storage");
_Static_assert(sizeof(ibeam_pointer_pixels) / sizeof(ibeam_pointer_pixels[0]) <= TERMINAL_POINTER_MAX_HEIGHT,
               "I-beam pointer is taller than its save-under storage");

static bool channel_valid(uint8_t size, uint8_t shift, uint16_t bpp) {
	return size != 0u && size <= 16u && shift < bpp && size <= bpp - shift;
}

static uint64_t channel_mask(uint8_t size, uint8_t shift) {
	return (((uint64_t)1u << size) - 1u) << shift;
}

static bool pixel_format_initialize(const struct framebuffer_info_response* info, struct pixel_format* format) {
	uint64_t red_mask;
	uint64_t green_mask;
	uint64_t blue_mask;

	if (info == NULL || format == NULL || info->memory_model != FRAMEBUFFER_MEMORY_MODEL_RGB || info->bpp == 0u ||
	    info->bpp > 64u || info->bpp % 8u != 0u ||
	    !channel_valid(info->red_mask_size, info->red_mask_shift, info->bpp) ||
	    !channel_valid(info->green_mask_size, info->green_mask_shift, info->bpp) ||
	    !channel_valid(info->blue_mask_size, info->blue_mask_shift, info->bpp))
		return false;
	red_mask   = channel_mask(info->red_mask_size, info->red_mask_shift);
	green_mask = channel_mask(info->green_mask_size, info->green_mask_shift);
	blue_mask  = channel_mask(info->blue_mask_size, info->blue_mask_shift);
	if ((red_mask & green_mask) != 0u || (red_mask & blue_mask) != 0u || (green_mask & blue_mask) != 0u) return false;
	*format = (struct pixel_format){
		.bytes_per_pixel = info->bpp / 8u,
		.red_size        = info->red_mask_size,
		.red_shift       = info->red_mask_shift,
		.green_size      = info->green_mask_size,
		.green_shift     = info->green_mask_shift,
		.blue_size       = info->blue_mask_size,
		.blue_shift      = info->blue_mask_shift,
	};
	return true;
}

static uint64_t encode_channel(uint8_t value, uint8_t size, uint8_t shift) {
	uint64_t maximum = ((uint64_t)1u << size) - 1u;
	return (((uint64_t)value * maximum + 127u) / 255u) << shift;
}

static uint64_t encode_color(const struct pixel_format* format, struct color color) {
	return encode_channel(color.red, format->red_size, format->red_shift) |
	       encode_channel(color.green, format->green_size, format->green_shift) |
	       encode_channel(color.blue, format->blue_size, format->blue_shift);
}

static void write_pixel(volatile uint8_t* destination, size_t size, uint64_t pixel) {
	for (size_t byte = 0u; byte < size; byte++) destination[byte] = (uint8_t)(pixel >> (byte * 8u));
}

static void hide_pointer_cursor(struct terminal* terminal) {
	const size_t pixel_size = terminal->surface.format.bytes_per_pixel;

	if (!terminal->pointer_drawn) return;
	for (size_t row = 0u; row < terminal->pointer_saved_height; row++) {
		volatile uint8_t* destination = terminal->surface.framebuffer +
		                                (terminal->pointer_saved_y + row) * terminal->surface.pitch +
		                                terminal->pointer_saved_x * pixel_size;
		const uint8_t*    source      = terminal->pointer_saved + row * terminal->pointer_saved_width * pixel_size;
		for (size_t byte = 0u; byte < terminal->pointer_saved_width * pixel_size; byte++)
			destination[byte] = source[byte];
	}
	terminal->pointer_drawn = false;
}

static void show_pointer_cursor(struct terminal* terminal) {
	const struct terminal_pointer_shape* shape      = &ibeam_pointer_shape;
	const size_t                         pixel_size = terminal->surface.format.bytes_per_pixel;
	size_t                               source_x   = 0u;
	size_t                               source_y   = 0u;
	size_t                               destination_x;
	size_t                               destination_y;
	size_t                               width;
	size_t                               height;
	uint64_t                             outline = encode_color(&terminal->surface.format, theme.cursor_text);
	uint64_t                             fill    = encode_color(&terminal->surface.format, theme.cursor);

	if (!terminal->pointer_moved || terminal->pointer_drawn) return;
	if (terminal->pointer_x < shape->hotspot_x) source_x = shape->hotspot_x - terminal->pointer_x;
	if (terminal->pointer_y < shape->hotspot_y) source_y = shape->hotspot_y - terminal->pointer_y;
	destination_x = terminal->pointer_x < shape->hotspot_x ? 0u : terminal->pointer_x - shape->hotspot_x;
	destination_y = terminal->pointer_y < shape->hotspot_y ? 0u : terminal->pointer_y - shape->hotspot_y;
	width         = shape->width - source_x;
	height        = shape->height - source_y;
	if (width > terminal->surface.width - destination_x) width = terminal->surface.width - destination_x;
	if (height > terminal->surface.height - destination_y) height = terminal->surface.height - destination_y;

	terminal->pointer_saved_x      = destination_x;
	terminal->pointer_saved_y      = destination_y;
	terminal->pointer_saved_width  = width;
	terminal->pointer_saved_height = height;
	for (size_t row = 0u; row < height; row++) {
		volatile uint8_t* destination = terminal->surface.framebuffer +
		                                (destination_y + row) * terminal->surface.pitch + destination_x * pixel_size;
		uint8_t*          saved       = terminal->pointer_saved + row * width * pixel_size;
		for (size_t column = 0u; column < width; column++) {
			for (size_t byte = 0u; byte < pixel_size; byte++)
				saved[column * pixel_size + byte] = destination[column * pixel_size + byte];
			char pixel = shape->pixels[(source_y + row) * shape->stride + source_x + column];
			if (pixel == 'B') write_pixel(destination + column * pixel_size, pixel_size, outline);
			else if (pixel == 'W') write_pixel(destination + column * pixel_size, pixel_size, fill);
		}
	}
	terminal->pointer_drawn = true;
}

static void fill_rectangle(volatile uint8_t* framebuffer, size_t pitch, const struct pixel_format* format, size_t x,
                           size_t y, size_t width, size_t height, struct color color) {
	uint64_t pixel = encode_color(format, color);
	for (size_t row = 0u; row < height; row++) {
		volatile uint8_t* destination = framebuffer + (y + row) * pitch + x * format->bytes_per_pixel;
		for (size_t column = 0u; column < width; column++) {
			write_pixel(destination, format->bytes_per_pixel, pixel);
			destination += format->bytes_per_pixel;
		}
	}
}

static void copy_framebuffer_row(volatile uint8_t* destination, const volatile uint8_t* source, size_t size) {
	if ((((uintptr_t)destination | (uintptr_t)source) & (sizeof(uint64_t) - 1u)) == 0u) {
		size_t words = size / sizeof(uint64_t);
		for (size_t word = 0u; word < words; word++)
			((volatile uint64_t*)destination)[word] = ((const volatile uint64_t*)source)[word];
		destination += words * sizeof(uint64_t);
		source += words * sizeof(uint64_t);
		size -= words * sizeof(uint64_t);
	}
	for (size_t byte = 0u; byte < size; byte++) destination[byte] = source[byte];
}

static struct color blend_color(struct color foreground, struct color background, uint8_t alpha) {
	uint16_t inverse = (uint16_t)(255u - alpha);
	return (struct color){
		.red   = (uint8_t)(((uint16_t)foreground.red * alpha + (uint16_t)background.red * inverse + 127u) / 255u),
		.green = (uint8_t)(((uint16_t)foreground.green * alpha + (uint16_t)background.green * inverse + 127u) / 255u),
		.blue  = (uint8_t)(((uint16_t)foreground.blue * alpha + (uint16_t)background.blue * inverse + 127u) / 255u),
	};
}

static bool draw_glyph(const struct drawing_surface* surface, const struct terminal_glyph* glyph, int64_t x, int64_t y,
                       size_t clip_x, size_t clip_y, size_t clip_width, size_t clip_height, struct color foreground,
                       struct color background) {
	size_t clip_right;
	size_t clip_bottom;

	if (surface == NULL || glyph == NULL || clip_x > surface->width || clip_y > surface->height ||
	    clip_width > surface->width - clip_x || clip_height > surface->height - clip_y)
		return false;
	clip_right  = clip_x + clip_width;
	clip_bottom = clip_y + clip_height;
	if (glyph->width == 0u || glyph->height == 0u) return true;
	if (glyph->alpha == NULL) return false;

	for (size_t row = 0u; row < glyph->height; row++) {
		for (size_t column = 0u; column < glyph->width; column++) {
			int64_t destination_x = x + (int64_t)column;
			int64_t destination_y = y + (int64_t)row;
			uint8_t alpha         = glyph->alpha[row * glyph->width + column];

			if (alpha == 0u || destination_x < 0 || destination_y < 0 || (uint64_t)destination_x < clip_x ||
			    (uint64_t)destination_x >= clip_right || (uint64_t)destination_y < clip_y ||
			    (uint64_t)destination_y >= clip_bottom)
				continue;
			volatile uint8_t* destination = surface->framebuffer + (size_t)destination_y * surface->pitch +
			                                (size_t)destination_x * surface->format.bytes_per_pixel;
			write_pixel(destination,
			            surface->format.bytes_per_pixel,
			            encode_color(&surface->format, blend_color(foreground, background, alpha)));
		}
	}
	return true;
}

static bool load_font(uint8_t** data, size_t* size) {
	FILE*    file = NULL;
	long     file_size;
	uint8_t* buffer  = NULL;
	bool     success = false;

	if (data == NULL || size == NULL) return false;
	*data = NULL;
	*size = 0u;
	file  = fopen(TERMINAL_FONT_PATH, "rb");
	if (file == NULL || fseek(file, 0l, SEEK_END) != 0) goto cleanup;
	file_size = ftell(file);
	if (file_size <= 0l || fseek(file, 0l, SEEK_SET) != 0) goto cleanup;
	buffer = malloc((size_t)file_size);
	if (buffer == NULL || fread(buffer, 1u, (size_t)file_size, file) != (size_t)file_size) goto cleanup;
	*data   = buffer;
	*size   = (size_t)file_size;
	buffer  = NULL;
	success = true;

cleanup:
	free(buffer);
	if (file != NULL && fclose(file) != 0) success = false;
	if (!success) {
		free(*data);
		*data = NULL;
		*size = 0u;
	}
	return success;
}

static void reset_attributes(struct terminal* terminal) {
	terminal->foreground_default = true;
	terminal->background_default = true;
	terminal->foreground         = 7u;
	terminal->background         = 0u;
	terminal->bold               = false;
	terminal->reverse            = false;
	terminal->underline          = false;
}

static struct color selected_foreground(const struct terminal* terminal) {
	struct color foreground =
		terminal->foreground_default
	        ? theme.foreground
	        : theme.palette[terminal->foreground < 8u && terminal->bold ? terminal->foreground + 8u
	                                                                    : terminal->foreground];
	struct color background = terminal->background_default ? theme.background : theme.palette[terminal->background];
	return terminal->reverse ? background : foreground;
}

static struct color selected_background(const struct terminal* terminal) {
	struct color foreground =
		terminal->foreground_default
	        ? theme.foreground
	        : theme.palette[terminal->foreground < 8u && terminal->bold ? terminal->foreground + 8u
	                                                                    : terminal->foreground];
	struct color background = terminal->background_default ? theme.background : theme.palette[terminal->background];
	return terminal->reverse ? foreground : background;
}

static struct terminal_cell blank_cell(const struct terminal* terminal) {
	return (struct terminal_cell){
		.codepoint  = ' ',
		.foreground = selected_foreground(terminal),
		.background = selected_background(terminal),
	};
}

static int position_compare(struct terminal_selection_position left, struct terminal_selection_position right) {
	if (left.line < right.line) return -1;
	if (left.line > right.line) return 1;
	if (left.column < right.column) return -1;
	if (left.column > right.column) return 1;
	return 0;
}

static void selection_bounds(const struct terminal_selection_state* selection,
                             struct terminal_selection_position* first, struct terminal_selection_position* last) {
	if (position_compare(selection->anchor, selection->focus) < 0) {
		*first = selection->anchor;
		*last  = selection->focus;
	}
	else {
		*first = selection->focus;
		*last  = selection->anchor;
	}
}

static bool selection_contains(const struct terminal_selection_state* selection, uint64_t line, size_t column) {
	struct terminal_selection_position position = {.line = line, .column = column};
	struct terminal_selection_position first;
	struct terminal_selection_position last;

	if (!selection->active) return false;
	selection_bounds(selection, &first, &last);
	return position_compare(position, first) >= 0 && position_compare(position, last) < 0;
}

static bool draw_cell_value(struct terminal* terminal, const struct terminal_cell* cell, size_t row, size_t column,
                            bool cursor, bool selected) {
	struct color foreground;
	struct color background;
	size_t       cell_x = column * terminal->cell_width;
	size_t       cell_y = row * terminal->cell_height;

	foreground = cursor ? theme.cursor_text : selected ? theme.selection_foreground : cell->foreground;
	background = cursor ? theme.cursor : selected ? theme.selection_background : cell->background;
	fill_rectangle(terminal->surface.framebuffer,
	               terminal->surface.pitch,
	               &terminal->surface.format,
	               cell_x,
	               cell_y,
	               terminal->cell_width,
	               terminal->cell_height,
	               background);
	if (cell->codepoint != ' ') {
		const struct terminal_glyph* glyph;
		int64_t                      glyph_x;
		int64_t                      glyph_y;

		if (!terminal_glyph_cache_get(terminal->glyph_cache, cell->codepoint, &glyph)) return false;
		glyph_x = (int64_t)cell_x + glyph->left;
		glyph_y = (int64_t)cell_y + (int64_t)terminal->ascender - glyph->top;
		if (!draw_glyph(&terminal->surface,
		                glyph,
		                glyph_x,
		                glyph_y,
		                cell_x,
		                cell_y,
		                terminal->cell_width,
		                terminal->cell_height,
		                foreground,
		                background))
			return false;
	}
	if (cell->underline && !cursor) {
		size_t underline_y = cell_y + terminal->ascender + 1u;
		if (underline_y >= cell_y + terminal->cell_height) underline_y = cell_y + terminal->cell_height - 1u;
		fill_rectangle(terminal->surface.framebuffer,
		               terminal->surface.pitch,
		               &terminal->surface.format,
		               cell_x,
		               underline_y,
		               terminal->cell_width,
		               1u,
		               foreground);
	}
	return true;
}

static bool live_row_visible(const struct terminal* terminal, size_t row) {
	return terminal->viewport_offset < terminal->rows && row < terminal->rows - terminal->viewport_offset;
}

static bool draw_cell(struct terminal* terminal, size_t row, size_t column, bool cursor) {
	if (terminal->viewport_offset != 0u) {
		if (live_row_visible(terminal, row)) terminal->viewport_dirty = true;
		return true;
	}
	return draw_cell_value(terminal,
	                       &terminal->cells[row * terminal->columns + column],
	                       row,
	                       column,
	                       cursor,
	                       selection_contains(&terminal->selection, terminal->screen_origin + row, column));
}

static const struct terminal_cell* history_line(const struct terminal* terminal, size_t line) {
	size_t slot = (terminal->history_head + line) % TERMINAL_SCROLLBACK_LINE_CAPACITY;
	return terminal->history + slot * terminal->columns;
}

static bool redraw_viewport_row(struct terminal* terminal, size_t row) {
	size_t                      stored_line = terminal->history_count - terminal->viewport_offset + row;
	uint64_t                    line        = terminal->screen_origin - terminal->viewport_offset + row;
	const struct terminal_cell* cells;

	if (stored_line < terminal->history_count) cells = history_line(terminal, stored_line);
	else cells = terminal->cells + (stored_line - terminal->history_count) * terminal->columns;
	for (size_t column = 0u; column < terminal->columns; column++)
		if (!draw_cell_value(
				terminal, &cells[column], row, column, false, selection_contains(&terminal->selection, line, column)))
			return false;
	return true;
}

static bool redraw_viewport(struct terminal* terminal) {
	for (size_t row = 0u; row < terminal->rows; row++)
		if (!redraw_viewport_row(terminal, row)) return false;
	terminal->viewport_dirty = false;
	return true;
}

static bool move_viewport(struct terminal* terminal, bool older, size_t lines) {
	size_t width  = terminal->columns * terminal->cell_width * terminal->surface.format.bytes_per_pixel;
	size_t height = terminal->rows * terminal->cell_height;
	size_t pixel_lines;

	if (lines >= terminal->rows || mul_overflow_size(lines, terminal->cell_height, &pixel_lines) ||
	    pixel_lines >= height)
		return redraw_viewport(terminal);
	if (older) {
		for (size_t row = height - pixel_lines; row != 0u; row--) {
			size_t            source_row = row - 1u;
			volatile uint8_t* destination =
				terminal->surface.framebuffer + (source_row + pixel_lines) * terminal->surface.pitch;
			volatile uint8_t* source = terminal->surface.framebuffer + source_row * terminal->surface.pitch;
			copy_framebuffer_row(destination, source, width);
		}
		for (size_t row = 0u; row < lines; row++)
			if (!redraw_viewport_row(terminal, row)) return false;
	}
	else {
		for (size_t row = 0u; row < height - pixel_lines; row++) {
			volatile uint8_t* destination = terminal->surface.framebuffer + row * terminal->surface.pitch;
			volatile uint8_t* source = terminal->surface.framebuffer + (row + pixel_lines) * terminal->surface.pitch;
			copy_framebuffer_row(destination, source, width);
		}
		for (size_t row = terminal->rows - lines; row < terminal->rows; row++)
			if (!redraw_viewport_row(terminal, row)) return false;
	}
	terminal->viewport_dirty = false;
	return true;
}

static const struct terminal_cell* overlay_cell_at(const struct terminal* terminal, size_t index) {
	for (size_t i = 0u; i < terminal->overlay_count; i++)
		if (terminal->overlay[i].index == index) return &terminal->overlay[i].cell;
	return NULL;
}

static bool redraw_viewport_cell(struct terminal* terminal, size_t row, size_t column) {
	size_t                      stored_line = terminal->history_count - terminal->viewport_offset + row;
	uint64_t                    line        = terminal->screen_origin - terminal->viewport_offset + row;
	const struct terminal_cell* cell;

	if (stored_line < terminal->history_count) cell = &history_line(terminal, stored_line)[column];
	else {
		size_t live_row = stored_line - terminal->history_count;
		cell            = &terminal->cells[live_row * terminal->columns + column];
		if (terminal->viewport_offset == 0u) {
			const struct terminal_cell* overlay = overlay_cell_at(terminal, live_row * terminal->columns + column);
			if (overlay != NULL) cell = overlay;
		}
	}
	return draw_cell_value(terminal, cell, row, column, false, selection_contains(&terminal->selection, line, column));
}

static bool redraw_selection_range(struct terminal* terminal, struct terminal_selection_position first,
                                   struct terminal_selection_position last) {
	uint64_t visible_first = terminal->screen_origin - terminal->viewport_offset;
	uint64_t visible_last =
		terminal->rows - 1u > UINT64_MAX - visible_first ? UINT64_MAX : visible_first + terminal->rows - 1u;
	uint64_t first_line;
	uint64_t last_line;

	if (position_compare(first, last) >= 0 || last.line < visible_first || first.line > visible_last) return true;
	first_line = first.line < visible_first ? visible_first : first.line;
	last_line  = last.line > visible_last ? visible_last : last.line;
	for (uint64_t line = first_line;; line++) {
		size_t row          = (size_t)(line - visible_first);
		size_t first_column = line == first.line ? first.column : 0u;
		size_t last_column  = line == last.line ? last.column : terminal->columns;

		if (first_column > terminal->columns) first_column = terminal->columns;
		if (last_column > terminal->columns) last_column = terminal->columns;
		for (size_t column = first_column; column < last_column; column++) {
			if (terminal->cursor_drawn && row == terminal->cursor_drawn_row && column == terminal->cursor_drawn_column)
				continue;
			if (!redraw_viewport_cell(terminal, row, column)) return false;
		}
		if (line == last_line) break;
	}
	return true;
}

static bool redraw_selection_difference(struct terminal* terminal, const struct terminal_selection_state* previous) {
	struct terminal_selection_position previous_first;
	struct terminal_selection_position previous_last;
	struct terminal_selection_position next_first;
	struct terminal_selection_position next_last;

	if (!previous->active) {
		if (!terminal->selection.active) return true;
		selection_bounds(&terminal->selection, &next_first, &next_last);
		return redraw_selection_range(terminal, next_first, next_last);
	}
	selection_bounds(previous, &previous_first, &previous_last);
	if (!terminal->selection.active) return redraw_selection_range(terminal, previous_first, previous_last);
	selection_bounds(&terminal->selection, &next_first, &next_last);
	if (position_compare(previous_last, next_first) <= 0 || position_compare(next_last, previous_first) <= 0)
		return redraw_selection_range(terminal, previous_first, previous_last) &&
		       redraw_selection_range(terminal, next_first, next_last);
	if (position_compare(previous_first, next_first) < 0) {
		if (!redraw_selection_range(terminal, previous_first, next_first)) return false;
	}
	else if (position_compare(next_first, previous_first) < 0 &&
	         !redraw_selection_range(terminal, next_first, previous_first))
		return false;
	if (position_compare(previous_last, next_last) < 0)
		return redraw_selection_range(terminal, previous_last, next_last);
	if (position_compare(next_last, previous_last) < 0)
		return redraw_selection_range(terminal, next_last, previous_last);
	return true;
}

static bool redraw_screen_cell(struct terminal* terminal, size_t row, size_t column, bool cursor) {
	size_t                      index = row * terminal->columns + column;
	const struct terminal_cell* cell  = overlay_cell_at(terminal, index);
	return cell == NULL
			   ? draw_cell(terminal, row, column, cursor)
			   : draw_cell_value(terminal,
			                     cell,
			                     row,
			                     column,
			                     cursor,
			                     selection_contains(&terminal->selection, terminal->screen_origin + row, column));
}

static bool hide_cursor(struct terminal* terminal) {
	if (!terminal->cursor_drawn) return true;
	terminal->cursor_drawn = false;
	return redraw_screen_cell(terminal, terminal->cursor_drawn_row, terminal->cursor_drawn_column, false);
}

static bool show_cursor(struct terminal* terminal) {
	size_t row    = terminal->row;
	size_t column = terminal->column;
	if (!terminal->cursor_visible || terminal->cursor_drawn || terminal->viewport_offset != 0u) return true;
	if (terminal->overlay_count != 0u) {
		size_t caret =
			terminal->preedit_caret < terminal->overlay_count ? terminal->preedit_caret : terminal->overlay_count;
		size_t index = terminal->overlay[0].index + caret;
		if (index >= terminal->cell_count) index = terminal->cell_count - 1u;
		row    = index / terminal->columns;
		column = index % terminal->columns;
	}
	if (!redraw_screen_cell(terminal, row, column, true)) return false;
	terminal->cursor_drawn        = true;
	terminal->cursor_drawn_row    = row;
	terminal->cursor_drawn_column = column;
	return true;
}

static bool erase_overlay(struct terminal* terminal) {
	bool success = true;
	for (size_t i = 0u; i < terminal->overlay_count; i++) {
		size_t index = terminal->overlay[i].index;
		if (terminal->viewport_offset == 0u &&
		    !draw_cell(terminal, index / terminal->columns, index % terminal->columns, false))
			success = false;
	}
	terminal->overlay_count = 0u;
	return success;
}

static bool draw_overlay(struct terminal* terminal) {
	if (terminal->preedit_count == 0u) return true;
	size_t first = terminal->row * terminal->columns + terminal->column;
	if (terminal->wrap_pending) {
		if (first + 1u >= terminal->cell_count) return true;
		first++;
	}
	terminal->overlay_count = terminal->preedit_count;
	if (terminal->overlay_count > terminal->cell_count - first) terminal->overlay_count = terminal->cell_count - first;
	for (size_t i = 0u; i < terminal->overlay_count; i++) {
		terminal->overlay[i] = (struct terminal_overlay_cell){
			.index = first + i,
			.cell =
				{
					   .codepoint  = terminal->preedit_codepoints[i],
					   .foreground = selected_foreground(terminal),
					   .background = selected_background(terminal),
					   .underline  = true,
					   },
		};
		size_t row    = terminal->overlay[i].index / terminal->columns;
		size_t column = terminal->overlay[i].index % terminal->columns;
		if (terminal->viewport_offset == 0u &&
		    !draw_cell_value(terminal,
		                     &terminal->overlay[i].cell,
		                     row,
		                     column,
		                     false,
		                     selection_contains(&terminal->selection, terminal->screen_origin + row, column)))
			return false;
	}
	return true;
}

static void terminal_lock(struct terminal* terminal) {
	while (__atomic_test_and_set(&terminal->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void terminal_unlock(struct terminal* terminal) {
	__atomic_clear(&terminal->locked, __ATOMIC_RELEASE);
}

static void clear_display(struct terminal* terminal) {
	struct terminal_cell blank = blank_cell(terminal);

	if (terminal->viewport_offset == 0u)
		fill_rectangle(terminal->surface.framebuffer,
		               terminal->surface.pitch,
		               &terminal->surface.format,
		               0u,
		               0u,
		               terminal->surface.width,
		               terminal->surface.height,
		               blank.background);
	else if (terminal->viewport_offset < terminal->rows) terminal->viewport_dirty = true;
	for (size_t index = 0u; index < terminal->cell_count; index++) terminal->cells[index] = blank;
	terminal->column       = 0u;
	terminal->row          = 0u;
	terminal->wrap_pending = false;
}

static void append_history_line(struct terminal* terminal, const struct terminal_cell* cells) {
	bool   full = terminal->history_count == TERMINAL_SCROLLBACK_LINE_CAPACITY;
	size_t slot;

	if (full) {
		slot                   = terminal->history_head;
		terminal->history_head = (terminal->history_head + 1u) % TERMINAL_SCROLLBACK_LINE_CAPACITY;
	}
	else {
		slot = (terminal->history_head + terminal->history_count) % TERMINAL_SCROLLBACK_LINE_CAPACITY;
		terminal->history_count++;
	}
	memcpy(terminal->history + slot * terminal->columns, cells, terminal->columns * sizeof(*cells));
	if (terminal->viewport_offset == 0u) return;
	if (!full || terminal->viewport_offset < terminal->history_count) terminal->viewport_offset++;
	else terminal->viewport_dirty = true;
}

static void expire_evicted_selection(struct terminal* terminal) {
	uint64_t first = terminal->screen_origin - terminal->history_count;

	if (terminal->pointer_selecting && terminal->pointer_selection_anchor.line < first) {
		terminal->pointer_primary_pressed  = false;
		terminal->pointer_selecting        = false;
		terminal->pointer_selection_source = 0u;
	}
	if (!terminal->selection.active ||
	    (terminal->selection.anchor.line >= first && terminal->selection.focus.line >= first))
		return;
	terminal->selection.active = false;
	terminal->viewport_dirty   = true;
}

static void scroll_screen(struct terminal* terminal) {
	size_t               width  = terminal->columns * terminal->cell_width * terminal->surface.format.bytes_per_pixel;
	size_t               height = terminal->rows * terminal->cell_height;
	size_t               copied_rows = height - terminal->cell_height;
	struct terminal_cell blank       = blank_cell(terminal);

	append_history_line(terminal, terminal->cells);
	terminal->screen_origin++;
	expire_evicted_selection(terminal);
	if (terminal->viewport_offset == 0u) {
		for (size_t row = 0u; row < copied_rows; row++) {
			volatile uint8_t* destination = terminal->surface.framebuffer + row * terminal->surface.pitch;
			volatile uint8_t* source =
				terminal->surface.framebuffer + (row + terminal->cell_height) * terminal->surface.pitch;
			copy_framebuffer_row(destination, source, width);
		}
	}
	memmove(terminal->cells,
	        terminal->cells + terminal->columns,
	        (terminal->cell_count - terminal->columns) * sizeof(*terminal->cells));
	for (size_t index = terminal->cell_count - terminal->columns; index < terminal->cell_count; index++)
		terminal->cells[index] = blank;
	if (terminal->viewport_offset == 0u)
		fill_rectangle(terminal->surface.framebuffer,
		               terminal->surface.pitch,
		               &terminal->surface.format,
		               0u,
		               copied_rows,
		               terminal->columns * terminal->cell_width,
		               terminal->cell_height,
		               blank.background);
}

static void next_line(struct terminal* terminal) {
	terminal->column       = 0u;
	terminal->wrap_pending = false;
	if (terminal->row + 1u < terminal->rows) terminal->row++;
	else scroll_screen(terminal);
}

static bool draw_codepoint(struct terminal* terminal, uint32_t codepoint) {
	struct terminal_cell* cell;

	if (terminal->wrap_pending) next_line(terminal);
	cell  = &terminal->cells[terminal->row * terminal->columns + terminal->column];
	*cell = (struct terminal_cell){
		.codepoint  = codepoint,
		.foreground = selected_foreground(terminal),
		.background = selected_background(terminal),
		.underline  = terminal->underline,
	};
	if (!draw_cell(terminal, terminal->row, terminal->column, false)) return false;
	if (terminal->column + 1u == terminal->columns) terminal->wrap_pending = true;
	else terminal->column++;
	return true;
}

static bool emit_replacement(struct terminal* terminal) {
	terminal->utf8_value     = 0u;
	terminal->utf8_minimum   = 0u;
	terminal->utf8_remaining = 0u;
	return draw_codepoint(terminal, 0xfffdu);
}

static void csi_reset(struct terminal* terminal) {
	terminal->csi_parameter_count = 0u;
	terminal->csi_value           = 0u;
	terminal->csi_has_value       = false;
	terminal->csi_invalid         = false;
	terminal->csi_private         = false;
}

static void csi_push(struct terminal* terminal) {
	if (terminal->csi_parameter_count >= TERMINAL_CSI_PARAMETER_CAPACITY) {
		terminal->csi_invalid = true;
		return;
	}
	terminal->csi_parameters[terminal->csi_parameter_count++] = terminal->csi_has_value ? terminal->csi_value : 0u;
	terminal->csi_value                                       = 0u;
	terminal->csi_has_value                                   = false;
}

static void apply_sgr(struct terminal* terminal) {
	if (terminal->csi_parameter_count == 0u) csi_push(terminal);
	if (terminal->csi_invalid || terminal->csi_private) return;
	for (size_t index = 0u; index < terminal->csi_parameter_count; index++) {
		uint16_t parameter = terminal->csi_parameters[index];
		if (parameter == 0u) reset_attributes(terminal);
		else if (parameter == 1u) terminal->bold = true;
		else if (parameter == 4u) terminal->underline = true;
		else if (parameter == 7u) terminal->reverse = true;
		else if (parameter == 22u) terminal->bold = false;
		else if (parameter == 24u) terminal->underline = false;
		else if (parameter == 27u) terminal->reverse = false;
		else if (parameter >= 30u && parameter <= 37u) {
			terminal->foreground_default = false;
			terminal->foreground         = (uint8_t)(parameter - 30u);
		}
		else if (parameter == 39u) terminal->foreground_default = true;
		else if (parameter >= 40u && parameter <= 47u) {
			terminal->background_default = false;
			terminal->background         = (uint8_t)(parameter - 40u);
		}
		else if (parameter == 49u) terminal->background_default = true;
		else if (parameter >= 90u && parameter <= 97u) {
			terminal->foreground_default = false;
			terminal->foreground         = (uint8_t)(parameter - 90u + 8u);
		}
		else if (parameter >= 100u && parameter <= 107u) {
			terminal->background_default = false;
			terminal->background         = (uint8_t)(parameter - 100u + 8u);
		}
		else if ((parameter == 38u || parameter == 48u) && index + 2u < terminal->csi_parameter_count &&
		         terminal->csi_parameters[index + 1u] == 5u && terminal->csi_parameters[index + 2u] < 16u) {
			if (parameter == 38u) {
				terminal->foreground_default = false;
				terminal->foreground         = (uint8_t)terminal->csi_parameters[index + 2u];
			}
			else {
				terminal->background_default = false;
				terminal->background         = (uint8_t)terminal->csi_parameters[index + 2u];
			}
			index += 2u;
		}
	}
}

static size_t csi_parameter(const struct terminal* terminal, size_t index, size_t default_value) {
	if (index >= terminal->csi_parameter_count || terminal->csi_parameters[index] == 0u) return default_value;
	return terminal->csi_parameters[index];
}

static bool erase_cells(struct terminal* terminal, size_t first, size_t last) {
	struct terminal_cell blank = blank_cell(terminal);

	if (first >= terminal->cell_count || last >= terminal->cell_count || first > last) return false;
	for (size_t index = first; index <= last; index++) {
		terminal->cells[index] = blank;
		if (!draw_cell(terminal, index / terminal->columns, index % terminal->columns, false)) return false;
	}
	return true;
}

static bool erase_scrollback(struct terminal* terminal) {
	terminal->history_head             = 0u;
	terminal->history_count            = 0u;
	terminal->viewport_offset          = 0u;
	terminal->selection                = (struct terminal_selection_state){0};
	terminal->pointer_primary_pressed  = false;
	terminal->pointer_selecting        = false;
	terminal->pointer_selection_source = 0u;
	terminal->pointer_selection_anchor = (struct terminal_selection_position){0};
	terminal->viewport_dirty           = false;
	return redraw_viewport(terminal);
}

static bool erase_display(struct terminal* terminal, size_t mode) {
	size_t cursor = terminal->row * terminal->columns + terminal->column;

	if (mode == 0u) return erase_cells(terminal, cursor, terminal->cell_count - 1u);
	if (mode == 1u) return erase_cells(terminal, 0u, cursor);
	if (mode == 2u) return erase_cells(terminal, 0u, terminal->cell_count - 1u);
	if (mode == 3u) return erase_scrollback(terminal);
	return true;
}

static bool erase_line(struct terminal* terminal, size_t mode) {
	size_t first = terminal->row * terminal->columns;
	size_t last  = first + terminal->columns - 1u;

	if (mode == 0u) first += terminal->column;
	else if (mode == 1u) last = first + terminal->column;
	else if (mode != 2u) return true;
	return erase_cells(terminal, first, last);
}

static void move_cursor(struct terminal* terminal, size_t row, size_t column) {
	terminal->row          = row < terminal->rows ? row : terminal->rows - 1u;
	terminal->column       = column < terminal->columns ? column : terminal->columns - 1u;
	terminal->wrap_pending = false;
}

static bool apply_csi(struct terminal* terminal, uint8_t final) {
	size_t amount;

	if (terminal->csi_invalid) return true;
	if (terminal->csi_private) {
		if ((final == 'h' || final == 'l') && terminal->csi_parameter_count == 1u && terminal->csi_parameters[0] == 25u)
			terminal->cursor_visible = final == 'h';
		return true;
	}
	if (final == 'm') {
		apply_sgr(terminal);
		return true;
	}
	amount = csi_parameter(terminal, 0u, 1u);
	if (final == 'A') {
		terminal->row          = amount > terminal->row ? 0u : terminal->row - amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'B') {
		terminal->row = amount >= terminal->rows - terminal->row ? terminal->rows - 1u : terminal->row + amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'C') {
		terminal->column =
			amount >= terminal->columns - terminal->column ? terminal->columns - 1u : terminal->column + amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'D') {
		terminal->column       = amount > terminal->column ? 0u : terminal->column - amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'E')
		move_cursor(
			terminal, amount >= terminal->rows - terminal->row ? terminal->rows - 1u : terminal->row + amount, 0u);
	else if (final == 'F') move_cursor(terminal, amount > terminal->row ? 0u : terminal->row - amount, 0u);
	else if (final == 'G') move_cursor(terminal, terminal->row, amount - 1u);
	else if (final == 'd') move_cursor(terminal, amount - 1u, terminal->column);
	else if (final == 'H' || final == 'f')
		move_cursor(terminal, csi_parameter(terminal, 0u, 1u) - 1u, csi_parameter(terminal, 1u, 1u) - 1u);
	else if (final == 'J') return erase_display(terminal, csi_parameter(terminal, 0u, 0u));
	else if (final == 'K') return erase_line(terminal, csi_parameter(terminal, 0u, 0u));
	else if (final == 's') {
		terminal->saved_row    = terminal->row;
		terminal->saved_column = terminal->column;
		terminal->saved_cursor = true;
	}
	else if (final == 'u' && terminal->saved_cursor) move_cursor(terminal, terminal->saved_row, terminal->saved_column);
	return true;
}

static bool process_text_byte(struct terminal* terminal, uint8_t byte);

static bool process_escape_byte(struct terminal* terminal, uint8_t byte) {
	if (terminal->escape_state == ESCAPE_STATE_ESCAPE) {
		terminal->escape_state = byte == '[' ? ESCAPE_STATE_CSI : ESCAPE_STATE_TEXT;
		if (byte == '[') csi_reset(terminal);
		else if (byte == '7') {
			terminal->saved_row    = terminal->row;
			terminal->saved_column = terminal->column;
			terminal->saved_cursor = true;
		}
		else if (byte == '8' && terminal->saved_cursor)
			move_cursor(terminal, terminal->saved_row, terminal->saved_column);
		return true;
	}
	if (byte == '?' && terminal->csi_parameter_count == 0u && !terminal->csi_has_value) {
		terminal->csi_private = true;
		return true;
	}
	if (byte >= '0' && byte <= '9') {
		uint16_t digit = (uint16_t)(byte - '0');
		if (terminal->csi_value > (UINT16_MAX - digit) / 10u) terminal->csi_invalid = true;
		else terminal->csi_value = (uint16_t)(terminal->csi_value * 10u + digit);
		terminal->csi_has_value = true;
		return true;
	}
	if (byte == ';') {
		csi_push(terminal);
		return true;
	}
	if (byte >= 0x40u && byte <= 0x7eu) {
		if (terminal->csi_has_value || terminal->csi_parameter_count != 0u) csi_push(terminal);
		bool success           = apply_csi(terminal, byte);
		terminal->escape_state = ESCAPE_STATE_TEXT;
		return success;
	}
	if (byte < 0x20u || byte > 0x7eu) terminal->escape_state = ESCAPE_STATE_TEXT;
	return true;
}

static bool process_utf8_byte(struct terminal* terminal, uint8_t byte) {
	if (terminal->utf8_remaining != 0u) {
		if ((byte & 0xc0u) != 0x80u) {
			if (!emit_replacement(terminal)) return false;
			return process_text_byte(terminal, byte);
		}
		terminal->utf8_value = (terminal->utf8_value << 6u) | (byte & 0x3fu);
		terminal->utf8_remaining--;
		if (terminal->utf8_remaining != 0u) return true;
		uint32_t codepoint     = terminal->utf8_value;
		uint32_t minimum       = terminal->utf8_minimum;
		terminal->utf8_value   = 0u;
		terminal->utf8_minimum = 0u;
		if (codepoint < minimum || codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu))
			return draw_codepoint(terminal, 0xfffdu);
		return draw_codepoint(terminal, codepoint);
	}
	if (byte < 0x80u) return draw_codepoint(terminal, byte);
	if (byte >= 0xc2u && byte <= 0xdfu) {
		terminal->utf8_value     = byte & 0x1fu;
		terminal->utf8_minimum   = 0x80u;
		terminal->utf8_remaining = 1u;
		return true;
	}
	if (byte >= 0xe0u && byte <= 0xefu) {
		terminal->utf8_value     = byte & 0x0fu;
		terminal->utf8_minimum   = 0x800u;
		terminal->utf8_remaining = 2u;
		return true;
	}
	if (byte >= 0xf0u && byte <= 0xf4u) {
		terminal->utf8_value     = byte & 0x07u;
		terminal->utf8_minimum   = 0x10000u;
		terminal->utf8_remaining = 3u;
		return true;
	}
	return draw_codepoint(terminal, 0xfffdu);
}

static bool process_text_byte(struct terminal* terminal, uint8_t byte) {
	if (terminal->escape_state != ESCAPE_STATE_TEXT) return process_escape_byte(terminal, byte);
	if (byte == 0x1bu) {
		if (terminal->utf8_remaining != 0u && !emit_replacement(terminal)) return false;
		terminal->escape_state = ESCAPE_STATE_ESCAPE;
		return true;
	}
	if (byte < 0x20u || byte == 0x7fu) {
		if (terminal->utf8_remaining != 0u && !emit_replacement(terminal)) return false;
		if (byte == '\n') next_line(terminal);
		else if (byte == '\r') {
			terminal->column       = 0u;
			terminal->wrap_pending = false;
		}
		else if (byte == '\b') {
			if (terminal->wrap_pending) terminal->wrap_pending = false;
			else if (terminal->column != 0u) terminal->column--;
		}
		else if (byte == '\t') {
			size_t count = TERMINAL_TAB_WIDTH - terminal->column % TERMINAL_TAB_WIDTH;
			for (size_t index = 0u; index < count; index++) {
				if (!draw_codepoint(terminal, ' ')) return false;
			}
		}
		else if (byte == '\f') clear_display(terminal);
		return true;
	}
	return process_utf8_byte(terminal, byte);
}

static bool positive_metric_pixels(FT_Pos value, size_t* out_pixels) {
	uintmax_t raw;
	uintmax_t pixels;

	if (value <= 0 || out_pixels == NULL) return false;
	raw    = (uintmax_t)value;
	pixels = raw / 64u + (raw % 64u != 0u ? 1u : 0u);
	if (pixels > SIZE_MAX) return false;
	*out_pixels = (size_t)pixels;
	return true;
}

static bool descender_metric_pixels(FT_Pos value, size_t* out_pixels) {
	uintmax_t raw;
	uintmax_t pixels;

	if (value > 0 || out_pixels == NULL) return false;
	raw    = value == LONG_MIN ? (uintmax_t)LONG_MAX + 1u : (uintmax_t)-value;
	pixels = raw / 64u + (raw % 64u != 0u ? 1u : 0u);
	if (pixels > SIZE_MAX) return false;
	*out_pixels = (size_t)pixels;
	return true;
}

struct terminal* terminal_create(volatile uint8_t* framebuffer, const struct framebuffer_info_response* info) {
	struct terminal* terminal = NULL;
	size_t           font_size;
	size_t           metrics_height;
	size_t           descender;
	size_t           glyph_height;
	size_t           visible_row_size;
	size_t           visible_size;
	size_t           history_cell_count;
	FT_Pos           advance;

	if (framebuffer == NULL || info == NULL) return NULL;
	terminal = calloc(1u, sizeof(*terminal));
	if (terminal == NULL || !pixel_format_initialize(info, &terminal->surface.format) ||
	    (uint64_t)(size_t)info->width != info->width || (uint64_t)(size_t)info->height != info->height ||
	    (uint64_t)(size_t)info->pitch != info->pitch || (uint64_t)(size_t)info->size != info->size)
		goto fail;
	terminal->surface = (struct drawing_surface){
		.framebuffer = framebuffer,
		.width       = (size_t)info->width,
		.height      = (size_t)info->height,
		.pitch       = (size_t)info->pitch,
		.format      = terminal->surface.format,
	};
	if (terminal->surface.width == 0u || terminal->surface.height == 0u ||
	    mul_overflow_size(terminal->surface.width, terminal->surface.format.bytes_per_pixel, &visible_row_size) ||
	    visible_row_size > terminal->surface.pitch ||
	    mul_overflow_size(terminal->surface.height, terminal->surface.pitch, &visible_size) ||
	    visible_size > (size_t)info->size || FT_Init_FreeType(&terminal->library) != FT_Err_Ok ||
	    !load_font(&terminal->font_data, &font_size) || font_size > LONG_MAX ||
	    FT_New_Memory_Face(terminal->library, terminal->font_data, (FT_Long)font_size, 0, &terminal->face) !=
	        FT_Err_Ok ||
	    FT_Set_Pixel_Sizes(terminal->face, 0u, TERMINAL_FONT_PIXEL_HEIGHT) != FT_Err_Ok ||
	    terminal->face->size->metrics.height <= 0 || terminal->face->size->metrics.ascender <= 0 ||
	    terminal->face->size->metrics.descender > 0 || FT_Load_Char(terminal->face, 'M', FT_LOAD_DEFAULT) != FT_Err_Ok)
		goto fail;
	advance = terminal->face->glyph->advance.x;
	if (advance <= 0) goto fail;
	if (!positive_metric_pixels(advance, &terminal->cell_width) ||
	    !positive_metric_pixels(terminal->face->size->metrics.height, &metrics_height) ||
	    !positive_metric_pixels(terminal->face->size->metrics.ascender, &terminal->ascender) ||
	    !descender_metric_pixels(terminal->face->size->metrics.descender, &descender) ||
	    add_overflow_size(terminal->ascender, descender, &glyph_height))
		goto fail;
	terminal->cell_height = metrics_height > glyph_height ? metrics_height : glyph_height;
	if (terminal->cell_width == 0u || terminal->cell_height == 0u || terminal->ascender > terminal->cell_height)
		goto fail;
	terminal->columns = terminal->surface.width / terminal->cell_width;
	terminal->rows    = terminal->surface.height / terminal->cell_height;
	if (terminal->columns == 0u || terminal->rows == 0u || terminal->rows > UINT64_MAX - 1u ||
	    mul_overflow_size(terminal->columns, terminal->rows, &terminal->cell_count) ||
	    terminal->cell_count > SIZE_MAX / sizeof(*terminal->cells) ||
	    mul_overflow_size(terminal->columns, TERMINAL_SCROLLBACK_LINE_CAPACITY, &history_cell_count) ||
	    history_cell_count > SIZE_MAX / sizeof(*terminal->history))
		goto fail;
	terminal->cells       = calloc(terminal->cell_count, sizeof(*terminal->cells));
	terminal->history     = malloc(history_cell_count * sizeof(*terminal->history));
	terminal->glyph_cache = terminal_glyph_cache_create(terminal->face);
	if (terminal->cells == NULL || terminal->history == NULL || terminal->glyph_cache == NULL) goto fail;
	terminal->screen_origin = 1u;
	reset_attributes(terminal);
	terminal->cursor_visible = true;
	terminal->pointer_x      = terminal->surface.width / 2u;
	terminal->pointer_y      = terminal->surface.height / 2u;
	clear_display(terminal);
	if (!show_cursor(terminal)) goto fail;
	return terminal;

fail:
	terminal_destroy(terminal);
	return NULL;
}

void terminal_destroy(struct terminal* terminal) {
	if (terminal == NULL) return;
	terminal_glyph_cache_destroy(terminal->glyph_cache);
	if (terminal->face != NULL) (void)FT_Done_Face(terminal->face);
	if (terminal->library != NULL) (void)FT_Done_FreeType(terminal->library);
	free(terminal->font_data);
	free(terminal->cells);
	free(terminal->history);
	free(terminal->preedit_codepoints);
	free(terminal->overlay);
	free(terminal->clipboard);
	free(terminal);
}

bool terminal_get_dimensions(const struct terminal* terminal, size_t* out_columns, size_t* out_rows) {
	if (terminal == NULL || out_columns == NULL || out_rows == NULL || terminal->columns == 0u || terminal->rows == 0u)
		return false;
	*out_columns = terminal->columns;
	*out_rows    = terminal->rows;
	return true;
}

bool terminal_get_cursor_position(struct terminal* terminal, size_t* out_column, size_t* out_row, uint64_t* out_line) {
	if (terminal == NULL || out_column == NULL || out_row == NULL || out_line == NULL) return false;
	terminal_lock(terminal);
	*out_column = terminal->column;
	*out_row    = terminal->row;
	*out_line   = terminal->screen_origin + terminal->row;
	terminal_unlock(terminal);
	return true;
}

bool terminal_write(struct terminal* terminal, const uint8_t* data, size_t size, size_t* out_written) {
	size_t written = 0u;
	bool   success;

	if (out_written == NULL) return false;
	*out_written = 0u;
	if (terminal == NULL || (size != 0u && data == NULL)) return false;
	if (size == 0u) return true;
	terminal_lock(terminal);
	hide_pointer_cursor(terminal);
	if (!hide_cursor(terminal) || !erase_overlay(terminal)) {
		show_pointer_cursor(terminal);
		terminal_unlock(terminal);
		return false;
	}
	while (written < size && process_text_byte(terminal, data[written])) written++;
	*out_written = written;
	success      = (!terminal->viewport_dirty || redraw_viewport(terminal)) && draw_overlay(terminal) &&
	               show_cursor(terminal) && written == size;
	show_pointer_cursor(terminal);
	terminal_unlock(terminal);
	return success;
}

static bool decode_utf8(const uint8_t* text, size_t size, uint32_t* codepoints, size_t* out_count, size_t selection_end,
                        size_t* out_caret) {
	size_t offset = 0u;
	size_t count  = 0u;
	size_t caret  = SIZE_MAX;

	if (out_count == NULL || out_caret == NULL || selection_end > size ||
	    (size != 0u && (text == NULL || codepoints == NULL)))
		return false;
	while (offset < size) {
		uint8_t  first = text[offset];
		uint32_t value;
		size_t   length;
		if (offset == selection_end) caret = count;
		if (first < 0x80u) {
			value  = first;
			length = 1u;
		}
		else if (first >= 0xc2u && first <= 0xdfu) {
			value  = first & 0x1fu;
			length = 2u;
		}
		else if (first >= 0xe0u && first <= 0xefu) {
			value  = first & 0x0fu;
			length = 3u;
		}
		else if (first >= 0xf0u && first <= 0xf4u) {
			value  = first & 0x07u;
			length = 4u;
		}
		else return false;
		if (length > size - offset || (selection_end > offset && selection_end < offset + length)) return false;
		for (size_t i = 1u; i < length; i++) {
			if ((text[offset + i] & 0xc0u) != 0x80u) return false;
			value = (value << 6u) | (text[offset + i] & 0x3fu);
		}
		uint32_t minimum = length == 1u ? 0u : length == 2u ? 0x80u : length == 3u ? 0x800u : 0x10000u;
		if (value < minimum || value > 0x10ffffu || (value >= 0xd800u && value <= 0xdfffu)) return false;
		codepoints[count++] = value;
		offset += length;
	}
	if (selection_end == size) caret = count;
	if (caret == SIZE_MAX) return false;
	*out_count = count;
	*out_caret = caret;
	return true;
}

bool terminal_set_preedit(struct terminal* terminal, const uint8_t* text, size_t size, size_t selection_end) {
	uint32_t*                     codepoints = NULL;
	struct terminal_overlay_cell* overlay    = NULL;
	size_t                        count;
	size_t                        caret;
	bool                          success;

	if (terminal == NULL || selection_end > size || (size != 0u && text == NULL) ||
	    size > SIZE_MAX / sizeof(*codepoints) || size > SIZE_MAX / sizeof(*overlay))
		return false;
	if (size != 0u) {
		codepoints = malloc(size * sizeof(*codepoints));
		overlay    = malloc(size * sizeof(*overlay));
		if (codepoints == NULL || overlay == NULL ||
		    !decode_utf8(text, size, codepoints, &count, selection_end, &caret)) {
			free(codepoints);
			free(overlay);
			return false;
		}
	}
	else {
		count = 0u;
		caret = 0u;
	}

	terminal_lock(terminal);
	hide_pointer_cursor(terminal);
	success = hide_cursor(terminal) && erase_overlay(terminal);
	if (success) {
		free(terminal->preedit_codepoints);
		free(terminal->overlay);
		terminal->preedit_codepoints = codepoints;
		terminal->preedit_count      = count;
		terminal->preedit_caret      = caret;
		terminal->overlay            = overlay;
		codepoints                   = NULL;
		overlay                      = NULL;
		success                      = draw_overlay(terminal) && show_cursor(terminal);
	}
	show_pointer_cursor(terminal);
	terminal_unlock(terminal);
	free(codepoints);
	free(overlay);
	return success;
}

bool terminal_scroll_view(struct terminal* terminal, int64_t lines) {
	size_t amount;
	size_t offset;
	size_t moved;
	bool   success;

	if (terminal == NULL) return false;
	if (lines > 0) amount = (uint64_t)lines > SIZE_MAX ? SIZE_MAX : (size_t)lines;
	else {
		uint64_t magnitude = lines == INT64_MIN ? (uint64_t)INT64_MAX + 1u : (uint64_t)-lines;
		amount             = magnitude > SIZE_MAX ? SIZE_MAX : (size_t)magnitude;
	}
	terminal_lock(terminal);
	hide_pointer_cursor(terminal);
	if (!hide_cursor(terminal) || !erase_overlay(terminal)) {
		show_pointer_cursor(terminal);
		terminal_unlock(terminal);
		return false;
	}
	offset = terminal->viewport_offset;
	if (lines > 0)
		terminal->viewport_offset =
			amount > terminal->history_count - offset ? terminal->history_count : offset + amount;
	else if (lines < 0) terminal->viewport_offset = amount > offset ? 0u : offset - amount;
	moved =
		terminal->viewport_offset > offset ? terminal->viewport_offset - offset : offset - terminal->viewport_offset;
	if (terminal->viewport_dirty) success = redraw_viewport(terminal);
	else success = moved == 0u || move_viewport(terminal, terminal->viewport_offset > offset, moved);
	success = success && draw_overlay(terminal) && show_cursor(terminal);
	show_pointer_cursor(terminal);
	terminal_unlock(terminal);
	return success;
}

bool terminal_scroll_to_bottom(struct terminal* terminal) {
	size_t offset;
	bool   success;

	if (terminal == NULL) return false;
	terminal_lock(terminal);
	if (terminal->viewport_offset == 0u) {
		terminal_unlock(terminal);
		return true;
	}
	hide_pointer_cursor(terminal);
	if (!hide_cursor(terminal) || !erase_overlay(terminal)) {
		show_pointer_cursor(terminal);
		terminal_unlock(terminal);
		return false;
	}
	offset                    = terminal->viewport_offset;
	terminal->viewport_offset = 0u;
	if (terminal->viewport_dirty) success = redraw_viewport(terminal);
	else success = move_viewport(terminal, false, offset);
	success = success && draw_overlay(terminal) && show_cursor(terminal);
	show_pointer_cursor(terminal);
	terminal_unlock(terminal);
	return success;
}

bool terminal_selection_get(struct terminal* terminal, struct terminal_get_selection_response* out_selection) {
	if (terminal == NULL || out_selection == NULL) return false;
	terminal_lock(terminal);
	if (!terminal->selection.active) {
		terminal_unlock(terminal);
		return false;
	}
	*out_selection = (struct terminal_get_selection_response){
		.anchor = terminal->selection.anchor,
		.focus  = terminal->selection.focus,
	};
	terminal_unlock(terminal);
	return true;
}

static bool selection_position_valid(const struct terminal* terminal, struct terminal_selection_position position) {
	uint64_t first = terminal->screen_origin - terminal->history_count;
	uint64_t last;

	if (terminal->rows - 1u > UINT64_MAX - terminal->screen_origin) return false;
	last = terminal->screen_origin + terminal->rows - 1u;
	return position.line >= first && position.line <= last && position.column <= terminal->columns;
}

static bool selection_state_equal(const struct terminal_selection_state* left,
                                  const struct terminal_selection_state* right) {
	return left->active == right->active && (!left->active || (position_compare(left->anchor, right->anchor) == 0 &&
	                                                           position_compare(left->focus, right->focus) == 0));
}

static bool set_selection_locked(struct terminal* terminal, struct terminal_selection_state next) {
	struct terminal_selection_state previous = terminal->selection;

	if (selection_state_equal(&previous, &next)) return true;
	terminal->selection = next;
	return redraw_selection_difference(terminal, &previous);
}

bool terminal_selection_set(struct terminal* terminal, struct terminal_selection_position anchor,
                            struct terminal_selection_position focus) {
	struct terminal_selection_state next = {.anchor = anchor, .focus = focus, .active = true};
	bool                            success;

	if (terminal == NULL || position_compare(anchor, focus) == 0) return false;
	terminal_lock(terminal);
	if (!selection_position_valid(terminal, anchor) || !selection_position_valid(terminal, focus)) {
		terminal_unlock(terminal);
		return false;
	}
	hide_pointer_cursor(terminal);
	success = set_selection_locked(terminal, next);
	show_pointer_cursor(terminal);
	terminal_unlock(terminal);
	return success;
}

bool terminal_selection_clear(struct terminal* terminal) {
	struct terminal_selection_state next = {0};
	bool                            success;

	if (terminal == NULL) return false;
	terminal_lock(terminal);
	if (!terminal->selection.active) {
		terminal_unlock(terminal);
		return true;
	}
	hide_pointer_cursor(terminal);
	success = set_selection_locked(terminal, next);
	show_pointer_cursor(terminal);
	terminal_unlock(terminal);
	return success;
}

static bool clipboard_codepoint_allowed(uint32_t codepoint) {
	return codepoint <= 0x10ffffu && !(codepoint >= 0xd800u && codepoint <= 0xdfffu) &&
	       (codepoint >= 0x20u || codepoint == '\t' || codepoint == '\n' || codepoint == '\r') &&
	       !(codepoint >= 0x7fu && codepoint <= 0x9fu);
}

bool terminal_clipboard_text_valid(const void* contents, size_t size) {
	const uint8_t* text   = contents;
	size_t         offset = 0u;

	if (size != 0u && text == NULL) return false;
	while (offset < size) {
		uint8_t  first = text[offset];
		uint32_t value;
		size_t   length;

		if (first < 0x80u) {
			value  = first;
			length = 1u;
		}
		else if (first >= 0xc2u && first <= 0xdfu) {
			value  = first & 0x1fu;
			length = 2u;
		}
		else if (first >= 0xe0u && first <= 0xefu) {
			value  = first & 0x0fu;
			length = 3u;
		}
		else if (first >= 0xf0u && first <= 0xf4u) {
			value  = first & 0x07u;
			length = 4u;
		}
		else return false;
		if (length > size - offset) return false;
		for (size_t i = 1u; i < length; i++) {
			if ((text[offset + i] & 0xc0u) != 0x80u) return false;
			value = (value << 6u) | (text[offset + i] & 0x3fu);
		}
		uint32_t minimum = length == 1u ? 0u : length == 2u ? 0x80u : length == 3u ? 0x800u : 0x10000u;
		if (value < minimum || !clipboard_codepoint_allowed(value)) return false;
		offset += length;
	}
	return true;
}

static size_t clipboard_codepoint_size(uint32_t codepoint) {
	if (!clipboard_codepoint_allowed(codepoint)) return 0u;
	if (codepoint < 0x80u) return 1u;
	if (codepoint < 0x800u) return 2u;
	if (codepoint < 0x10000u) return 3u;
	return 4u;
}

static size_t clipboard_encode_codepoint(uint8_t* output, uint32_t codepoint) {
	size_t size = clipboard_codepoint_size(codepoint);

	if (size == 1u) output[0] = (uint8_t)codepoint;
	else if (size == 2u) {
		output[0] = (uint8_t)(0xc0u | (codepoint >> 6u));
		output[1] = (uint8_t)(0x80u | (codepoint & 0x3fu));
	}
	else if (size == 3u) {
		output[0] = (uint8_t)(0xe0u | (codepoint >> 12u));
		output[1] = (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu));
		output[2] = (uint8_t)(0x80u | (codepoint & 0x3fu));
	}
	else if (size == 4u) {
		output[0] = (uint8_t)(0xf0u | (codepoint >> 18u));
		output[1] = (uint8_t)(0x80u | ((codepoint >> 12u) & 0x3fu));
		output[2] = (uint8_t)(0x80u | ((codepoint >> 6u) & 0x3fu));
		output[3] = (uint8_t)(0x80u | (codepoint & 0x3fu));
	}
	return size;
}

static const struct terminal_cell* retained_line_cells(const struct terminal* terminal, uint64_t line) {
	uint64_t first = terminal->screen_origin - terminal->history_count;

	if (line < first || line - first >= terminal->history_count + terminal->rows) return NULL;
	if (line < terminal->screen_origin) return history_line(terminal, (size_t)(line - first));
	return terminal->cells + (size_t)(line - terminal->screen_origin) * terminal->columns;
}

static size_t selected_line_end(const struct terminal_cell* cells, size_t first, size_t last) {
	while (last > first && cells[last - 1u].codepoint == ' ') last--;
	return last;
}

enum terminal_clipboard_copy_result terminal_clipboard_copy_selection(struct terminal* terminal) {
	struct terminal_selection_position first;
	struct terminal_selection_position last;
	uint8_t*                           clipboard      = NULL;
	size_t                             clipboard_size = 0u;
	size_t                             offset         = 0u;

	if (terminal == NULL) return TERMINAL_CLIPBOARD_COPY_FAILED;
	terminal_lock(terminal);
	if (!terminal->selection.active) {
		terminal_unlock(terminal);
		return TERMINAL_CLIPBOARD_COPY_UNAVAILABLE;
	}
	selection_bounds(&terminal->selection, &first, &last);
	for (uint64_t line = first.line;; line++) {
		const struct terminal_cell* cells = retained_line_cells(terminal, line);
		size_t                      begin = line == first.line ? (size_t)first.column : 0u;
		size_t                      end   = line == last.line ? (size_t)last.column : terminal->columns;

		if (cells == NULL || begin > terminal->columns || end > terminal->columns || begin > end) goto failed;
		end = selected_line_end(cells, begin, end);
		for (size_t column = begin; column < end; column++) {
			size_t encoded = clipboard_codepoint_size(cells[column].codepoint);
			if (encoded > TERMINAL_CLIPBOARD_CAPACITY - clipboard_size) goto failed;
			clipboard_size += encoded;
		}
		if (line == last.line) break;
		if (clipboard_size == TERMINAL_CLIPBOARD_CAPACITY) goto failed;
		clipboard_size++;
	}
	if (clipboard_size != 0u) {
		clipboard = malloc(clipboard_size);
		if (clipboard == NULL) goto failed;
	}
	for (uint64_t line = first.line;; line++) {
		const struct terminal_cell* cells = retained_line_cells(terminal, line);
		size_t                      begin = line == first.line ? (size_t)first.column : 0u;
		size_t                      end   = line == last.line ? (size_t)last.column : terminal->columns;

		end = selected_line_end(cells, begin, end);
		for (size_t column = begin; column < end; column++) {
			size_t encoded = clipboard_codepoint_size(cells[column].codepoint);
			if (encoded != 0u) offset += clipboard_encode_codepoint(clipboard + offset, cells[column].codepoint);
		}
		if (line == last.line) break;
		clipboard[offset++] = '\n';
	}
	free(terminal->clipboard);
	terminal->clipboard      = clipboard;
	terminal->clipboard_size = clipboard_size;
	terminal_unlock(terminal);
	return TERMINAL_CLIPBOARD_COPY_OK;

failed:
	terminal_unlock(terminal);
	free(clipboard);
	return TERMINAL_CLIPBOARD_COPY_FAILED;
}

bool terminal_clipboard_get(struct terminal* terminal, void* buffer, size_t capacity, size_t* out_size) {
	if (terminal == NULL || out_size == NULL || (capacity != 0u && buffer == NULL)) return false;
	terminal_lock(terminal);
	*out_size = terminal->clipboard_size;
	if (capacity < terminal->clipboard_size) {
		terminal_unlock(terminal);
		return false;
	}
	if (terminal->clipboard_size != 0u) memcpy(buffer, terminal->clipboard, terminal->clipboard_size);
	terminal_unlock(terminal);
	return true;
}

bool terminal_clipboard_snapshot(struct terminal* terminal, uint8_t** out_text, size_t* out_size) {
	uint8_t* text = NULL;

	if (terminal == NULL || out_text == NULL || out_size == NULL) return false;
	*out_text = NULL;
	*out_size = 0u;
	terminal_lock(terminal);
	if (terminal->clipboard_size != 0u) {
		text = malloc(terminal->clipboard_size);
		if (text == NULL) {
			terminal_unlock(terminal);
			return false;
		}
		memcpy(text, terminal->clipboard, terminal->clipboard_size);
	}
	*out_text = text;
	*out_size = terminal->clipboard_size;
	terminal_unlock(terminal);
	return true;
}

bool terminal_clipboard_set(struct terminal* terminal, const void* text, size_t size) {
	uint8_t* copy = NULL;

	if (terminal == NULL || size > TERMINAL_CLIPBOARD_CAPACITY || !terminal_clipboard_text_valid(text, size))
		return false;
	if (size != 0u) {
		copy = malloc(size);
		if (copy == NULL) return false;
		memcpy(copy, text, size);
	}
	terminal_lock(terminal);
	free(terminal->clipboard);
	terminal->clipboard      = copy;
	terminal->clipboard_size = size;
	terminal_unlock(terminal);
	return true;
}

static size_t move_pointer_axis(size_t current, size_t maximum, int64_t delta) {
	if (delta < 0) {
		uint64_t amount = delta == INT64_MIN ? (uint64_t)INT64_MAX + 1u : (uint64_t)-delta;
		return amount > current ? 0u : current - (size_t)amount;
	}
	uint64_t available = maximum - current;
	return (uint64_t)delta > available ? maximum : current + (size_t)delta;
}

static struct terminal_selection_position pointer_selection_position(const struct terminal* terminal) {
	size_t column = terminal->pointer_x / terminal->cell_width;
	size_t row    = terminal->pointer_y / terminal->cell_height;

	if (column >= terminal->columns) column = terminal->columns;
	else if (terminal->pointer_x % terminal->cell_width >= (terminal->cell_width + 1u) / 2u) column++;
	if (row >= terminal->rows) row = terminal->rows - 1u;
	return (struct terminal_selection_position){
		.line   = terminal->screen_origin - terminal->viewport_offset + row,
		.column = column,
	};
}

bool terminal_pointer_update(struct terminal* terminal, uint64_t source_id, int64_t delta_x, int64_t delta_y,
                             bool primary_changed, bool primary_pressed) {
	struct terminal_selection_state    next;
	struct terminal_selection_position position;
	bool                               success = true;

	if (terminal == NULL || source_id == 0u) return false;
	terminal_lock(terminal);
	hide_pointer_cursor(terminal);
	if (delta_x != 0 || delta_y != 0) terminal->pointer_moved = true;
	terminal->pointer_x = move_pointer_axis(terminal->pointer_x, terminal->surface.width - 1u, delta_x);
	terminal->pointer_y = move_pointer_axis(terminal->pointer_y, terminal->surface.height - 1u, delta_y);
	position            = pointer_selection_position(terminal);
	if (primary_changed && primary_pressed && !terminal->pointer_primary_pressed) {
		terminal->pointer_primary_pressed  = true;
		terminal->pointer_selection_source = source_id;
		terminal->pointer_selecting        = true;
		terminal->pointer_selection_anchor = position;
		next                               = (struct terminal_selection_state){0};
		success                            = set_selection_locked(terminal, next);
	}
	else if (primary_changed && !primary_pressed && terminal->pointer_primary_pressed &&
	         terminal->pointer_selection_source == source_id) {
		terminal->pointer_primary_pressed = false;
		if (terminal->pointer_selecting) {
			next = (struct terminal_selection_state){
				.anchor = terminal->pointer_selection_anchor,
				.focus  = position,
				.active = position_compare(terminal->pointer_selection_anchor, position) != 0,
			};
			success                     = set_selection_locked(terminal, next);
			terminal->pointer_selecting = false;
		}
		terminal->pointer_selection_source = 0u;
	}
	else if (terminal->pointer_selecting && terminal->pointer_selection_source == source_id) {
		next = (struct terminal_selection_state){
			.anchor = terminal->pointer_selection_anchor,
			.focus  = position,
			.active = position_compare(terminal->pointer_selection_anchor, position) != 0,
		};
		success = set_selection_locked(terminal, next);
	}
	show_pointer_cursor(terminal);
	terminal_unlock(terminal);
	return success;
}

void terminal_pointer_cancel(struct terminal* terminal, uint64_t source_id) {
	if (terminal == NULL) return;
	terminal_lock(terminal);
	if (source_id == 0u || source_id == terminal->pointer_selection_source) {
		terminal->pointer_primary_pressed  = false;
		terminal->pointer_selecting        = false;
		terminal->pointer_selection_source = 0u;
	}
	terminal_unlock(terminal);
}
