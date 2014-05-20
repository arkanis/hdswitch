#pragma once

#include <stdint.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "hash.h"
#include "array.h"

/**
 * Schema of the texture used by
 * the font renderer:
 * 
 * +--------------------+
 * | |cell||| | ||      | line
 * +-------------+      |
 * ||| | | ||           |
 * +--------+           |
 * |                    |
 * |                    |
 * |                    |
 * |                    |
 * +--------------------+
 * 
 * Lines are horizontal regions in the texture where cells of the same
 * height are stored.
 * A cell is a rectangle in a line that stores the pixel data of one
 * glyph. The cell structure also stores metrics so we don't have to ask
 * FreeType every time.
 */

typedef struct {
	GLuint texture;
	hash_p fonts;
	FT_Library freetype;
	array_p lines;
} text_renderer_t, *text_renderer_p;

void text_renderer_new(text_renderer_p renderer, size_t texture_width, size_t texture_height);
void text_renderer_destroy(text_renderer_p renderer);


typedef struct {
	FT_Face face;
	// Maps code points of this font to their corresponding cells. Only
	// cells already stored in the texture are in here.
	hash_p cell_refs;
} text_renderer_font_t, *text_renderer_font_p;

typedef struct {
	size_t line_idx, cell_idx;
} text_renderer_cell_ref_t, *text_renderer_cell_ref_p;

int32_t text_renderer_font_new(text_renderer_p renderer, const char* font_path, size_t font_size);
void    text_renderer_font_destroy(text_renderer_p renderer, int32_t font_handle);

void   text_renderer_prepare(text_renderer_p renderer, int32_t font_handle, uint32_t range_start, uint32_t range_end);
size_t text_renderer_render(text_renderer_p renderer, int32_t font_handle, char* text, size_t x, size_t y, float* buffer_ptr, size_t buffer_size);

typedef struct {
	uint32_t height;
	array_p cells;
} text_renderer_line_t, *text_renderer_line_p;

typedef struct {
	uint32_t x, y;
	uint32_t width;
	uint32_t glyph_index;
	int32_t hori_bearing_x, hori_bearing_y, hori_advance;
} text_renderer_cell_t, *text_renderer_cell_p;