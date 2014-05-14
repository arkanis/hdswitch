#include "drawable.h"
#include "text_renderer.h"


void text_renderer_new(text_renderer_p renderer, size_t texture_width, size_t texture_height) {
	FT_Error error = FT_Init_FreeType(&renderer->freetype);
	if (error) {
		printf("FT_Init_FreeType error\n");
		return;
	}
	
	renderer->texture = texture_new(texture_width, texture_height, GL_RGB8);
	renderer->fonts = hash_of(text_renderer_font_t);
}

void text_renderer_destroy(text_renderer_p renderer) {
	texture_destroy(renderer->texture);
	
	for(hash_elem_t e = hash_start(renderer->fonts); e != NULL; e = hash_next(renderer->fonts, e))
		text_renderer_font_destroy(renderer, hash_key(e));
	hash_destroy(renderer->fonts);
	
	FT_Error error = FT_Done_FreeType(renderer->freetype);
	if (error)
		printf("FT_Done_FreeType error\n");
}

int32_t text_renderer_font_new(text_renderer_p renderer, const char* font_path, size_t font_size) {
	int32_t handle = renderer->fonts->length;
	text_renderer_font_p font = hash_put_ptr(renderer->fonts, handle);
	
	FT_Error error = FT_New_Face(renderer->freetype, font_path, 0, &font->face);
	if (error){
		printf("FT_New_Face error\n");
		hash_remove(renderer->fonts, handle);
		return -1;
	}
	
	error = FT_Set_Pixel_Sizes(font->face, 0, font_size);
	if (error){
		printf("FT_Set_Pixel_Size error\n");
		FT_Done_Face(font->face);
		hash_remove(renderer->fonts, handle);
		return -1;
	}
	
	font->glyph_refs = hash_of(text_renderer_glyph_ref_t);
	
	return handle;
}

void text_renderer_font_destroy(text_renderer_p renderer, int32_t font_handle) {
	text_renderer_font_p font = hash_get_ptr(renderer->fonts, font_handle);
	if (font) {
		FT_Done_Face(font->face);
		hash_destroy(font->glyph_refs);
		hash_remove(renderer->fonts, font_handle);
	}
}

void text_renderer_render(text_renderer_p renderer, int32_t font_handle, const char* text, size_t x, size_t y, float* buffer_ptr, size_t buffer_size) {
	text_renderer_font_p font = hash_get_ptr(renderer->fonts, font_handle);
	if (!font)
		return;
	
	for(utf8_iterator_t it = utf8_first(text); it.code_point != 0; it = utf8_next(it)) {
		text_renderer_rect_ref_p rect_ref = hash_get_ptr(font->rect_refs, it.code_point);
		text_renderer_rect_t rect;
		
		if (rect_ref) {
			// Glyph already rendered, lookup the values
			rect = array_elem( array_elem(renderer->lines, text_renderer_line_t, rect_ref->line_idx).rects, rect_ref->rect_idx );
		} else {
			// Glyph not yet rendered, so do it
			FT_UInt glyph_index = FT_Get_Char_Index(font->face, it.code_point);
			
			error = FT_Load_Glyph(font->face, glyph_index, FT_LOAD_RENDER);
			if (error) {
				continue;
			}
			
			
			
			// Search for a free rect and store the glyph image there
			texture_update_part(renderer->texture, GL_RGB, 0, 0, )
		}
		
		if (glyph_index && prev_glyph_index) {
			FT_Vector delta;
			FT_Get_Kerning(hud->face, prev_glyph_index, glyph_index, FT_KERNING_DEFAULT, &delta);
			pen_x += delta.x >> 6;
		}
	}
}