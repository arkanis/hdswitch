#pragma once

#include <stdint.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "hash.h"
#include "array.h"


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
	hash_p glyph_refs;
} text_renderer_font_t, *text_renderer_font_p;

typedef struct {
	size_t line_idx, rect_idx;
} text_renderer_rect_ref_t, *text_renderer_rect_ref_p;

int32_t text_renderer_font_new(text_renderer_p renderer, const char* font_path, size_t font_size);
void    text_renderer_font_destroy(text_renderer_p renderer, int32_t font_handle);

void text_renderer_render(text_renderer_p renderer, int32_t font_handle, const char* text, size_t x, size_t y, float* buffer_ptr, size_t buffer_size);

typedef struct {
	uint32_t glyph_height;
	array_p rects;
} text_renderer_line_t, *text_renderer_line_p;

typedef struct {
	float tex_coord_x, tex_coord_y;
	uint32_t glyph_width;
	uint32_t hori_bearing_x, hori_bearing_y;
} text_renderer_rect_t, *text_renderer_rect_p;