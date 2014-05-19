#include "drawable.h"
#include "text_renderer.h"
#include "utf8.h"


void text_renderer_new(text_renderer_p renderer, size_t texture_width, size_t texture_height) {
	FT_Error error = FT_Init_FreeType(&renderer->freetype);
	if (error) {
		printf("FT_Init_FreeType error\n");
		return;
	}
	
	renderer->texture = texture_new(texture_width, texture_height, GL_R8);
	
	size_t black_data_size = texture_width * texture_height;
	void* black_data = malloc(black_data_size);
	memset(black_data, 0, black_data_size);
	texture_update(renderer->texture, GL_RED, black_data);
	free(black_data);
	
	glBindTexture(GL_TEXTURE_RECTANGLE, renderer->texture);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);	
	
	renderer->fonts = hash_of(text_renderer_font_t);
	renderer->lines = array_of(text_renderer_line_t);
}

void text_renderer_destroy(text_renderer_p renderer) {
	texture_destroy(renderer->texture);
	
	// TODO: destroy content of all lines
	array_destroy(renderer->lines);
	
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
	
	font->rect_refs = hash_of(text_renderer_rect_ref_t);
	
	return handle;
}

void text_renderer_font_destroy(text_renderer_p renderer, int32_t font_handle) {
	text_renderer_font_p font = hash_get_ptr(renderer->fonts, font_handle);
	if (font) {
		FT_Done_Face(font->face);
		hash_destroy(font->rect_refs);
		hash_remove(renderer->fonts, font_handle);
	}
}

size_t text_renderer_render(text_renderer_p renderer, int32_t font_handle, char* text, size_t x, size_t y, float* buffer_ptr, size_t buffer_size) {
	text_renderer_font_p font = hash_get_ptr(renderer->fonts, font_handle);
	if (!font)
		return 0;
	
	float* p = buffer_ptr;
	size_t org_x = x;
	int padding = 2;
	FT_UInt glyph_index = 0, prev_glyph_index = 0;
	
	glBindTexture(GL_TEXTURE_RECTANGLE, renderer->texture);
	
	GLint unpack_alignment = 0;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	
	for(utf8_iterator_t it = utf8_first(text); it.code_point != 0; it = utf8_next(it)) {
		if (it.code_point == '\n') {
			y += font->face->size->metrics.height / 64;
			x = org_x;
			continue;
		}
		
		text_renderer_rect_ref_p rect_ref = hash_get_ptr(font->rect_refs, it.code_point);
		text_renderer_rect_t rect;
		text_renderer_line_t rline;
		
		if (rect_ref) {
			// Glyph already rendered, lookup the values
			rline = array_elem(renderer->lines, text_renderer_line_t, rect_ref->line_idx);
			rect = array_elem(rline.rects, text_renderer_rect_t, rect_ref->rect_idx);
		} else {
			// Glyph not yet rendered, so do it
			glyph_index = FT_Get_Char_Index(font->face, it.code_point);
			
			FT_Error error = FT_Load_Glyph(font->face, glyph_index, FT_LOAD_RENDER /*| FT_LOAD_TARGET_LCD*/);
			if (!error) {
				float tex_x = 0, tex_y = 0;
				//uint32_t gw = (font->face->glyph->metrics.width  + 63) / 64;
				//uint32_t gh = (font->face->glyph->metrics.height + 63) / 64;
				// need to divide by 3 because of subpixel rendering
				//uint32_t gw = font->face->glyph->bitmap.width / 3;
				//uint32_t gh = font->face->glyph->bitmap.rows;
				uint32_t gw = font->face->glyph->bitmap.width, gh = font->face->glyph->bitmap.rows;
				
				// Search for a free slot to put the glyph image
				text_renderer_line_p line = NULL;
				for(size_t i = 0; i < renderer->lines->length; i++) {
					text_renderer_line_p current_line = &array_data(renderer->lines, text_renderer_line_t)[i];
					
					if (current_line->glyph_height == gh) {
						line = current_line;
						break;
					}
					
					tex_y += current_line->glyph_height + padding;
				}
				
				// Found no line with matching gylph height, so add a new one
				if (line == NULL) {
					text_renderer_line_t new_line;
					new_line.glyph_height = gh;
					new_line.rects = array_of(text_renderer_rect_t);
					
					array_append(renderer->lines, text_renderer_line_t, new_line);
					line = &array_data(renderer->lines, text_renderer_line_t)[renderer->lines->length - 1];
				}
				
				// TODO: Reuse free rects
				for(size_t i = 0; i < line->rects->length; i++) {
					text_renderer_rect_p current_rect = &array_data(line->rects, text_renderer_rect_t)[i];
					tex_x += current_rect->glyph_width + padding;
				}
				
				// Append a rect for this glyph
				text_renderer_rect_t new_rect;
				new_rect.tex_coord_x = tex_x;
				new_rect.tex_coord_y = tex_y;
				new_rect.glyph_width = gw;
				//new_rect.hori_bearing_x = font->face->glyph->bitmap_left;
				//new_rect.hori_bearing_y = font->face->glyph->bitmap_top;
				//new_rect.hori_advance   = font->face->glyph->advance.x / 64;
				new_rect.hori_bearing_x = font->face->glyph->metrics.horiBearingX / 64;
				new_rect.hori_bearing_y = font->face->glyph->metrics.horiBearingY / 64;
				new_rect.hori_advance   = font->face->glyph->metrics.horiAdvance  / 64;
				array_append(line->rects, text_renderer_rect_t, new_rect);
				
				rline = *line;
				rect = new_rect;
				
				// Store glyph image in new rect
				printf("%c: %2ux%2u %2u bytes, pitch %u, x: %zu, y: %zu hb: %3ld/%3ld, adv: %3ld/%3ld\n",
					it.code_point, gw, gh, gw*gh, font->face->glyph->bitmap.pitch,
					x, y,
					font->face->glyph->metrics.horiBearingX, font->face->glyph->metrics.horiBearingY,
					font->face->glyph->metrics.horiAdvance, font->face->glyph->metrics.vertAdvance);
				//void* test = malloc(gw*gh*3);
				//memset(test, 64, gw*gh*3);
					//texture_update_part(renderer->texture, GL_RGB, test, tex_x, tex_y, gw, gh, gw);
					glPixelStorei(GL_UNPACK_ROW_LENGTH, font->face->glyph->bitmap.pitch);
					glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, tex_x, tex_y, gw, gh, GL_RED, GL_UNSIGNED_BYTE, font->face->glyph->bitmap.buffer);
					
					//texture_update_part(renderer->texture, GL_RGB, font->face->glyph->bitmap.buffer, tex_x, tex_y, gw, gh, font->face->glyph->bitmap.pitch / 3);
					//texture_update_part(renderer->texture, GL_RGB, font->face->glyph->bitmap.buffer, tex_x, tex_y, gw, gh, font->face->glyph->bitmap.pitch);
				//free(test);
			}
		}
		
		if (glyph_index && prev_glyph_index) {
			FT_Vector delta;
			FT_Get_Kerning(font->face, prev_glyph_index, glyph_index, FT_KERNING_DEFAULT, &delta);
			x += delta.x / 64;
		}
		
		// We have the texture coordinates of the glyph, generate the vertex buffer
		if ( (p + 6*4 - buffer_ptr) * sizeof(float) < buffer_size ) {
			float w = rect.glyph_width, h = rline.glyph_height;
			
			float cx = x + rect.hori_bearing_x;
			float cy = y - rect.hori_bearing_y;
			
			float tl_x = cx + 0, tl_y = cy + 0,  tl_u = rect.tex_coord_x,     tl_v = rect.tex_coord_y;
			float tr_x = cx + w, tr_y = cy + 0,  tr_u = rect.tex_coord_x + w, tr_v = rect.tex_coord_y;
			float bl_x = cx + 0, bl_y = cy + h,  bl_u = rect.tex_coord_x,     bl_v = rect.tex_coord_y + h;
			float br_x = cx + w, br_y = cy + h,  br_u = rect.tex_coord_x + w, br_v = rect.tex_coord_y + h;
			
			*(p++) = tl_x; *(p++) = tl_y; *(p++) = tl_u; *(p++) = tl_v;
			*(p++) = tr_x; *(p++) = tr_y; *(p++) = tr_u; *(p++) = tr_v;
			*(p++) = bl_x; *(p++) = bl_y; *(p++) = bl_u; *(p++) = bl_v;
			*(p++) = tr_x; *(p++) = tr_y; *(p++) = tr_u; *(p++) = tr_v;
			*(p++) = br_x; *(p++) = br_y; *(p++) = br_u; *(p++) = br_v;
			*(p++) = bl_x; *(p++) = bl_y; *(p++) = bl_u; *(p++) = bl_v;
			
			x += rect.hori_advance;
		}
		
		prev_glyph_index = glyph_index;
	}
	
	glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	
	return (p - buffer_ptr) * sizeof(float);
}

