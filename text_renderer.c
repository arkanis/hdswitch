#include "drawable.h"
#include "text_renderer.h"
#include "utf8.h"

static text_renderer_cell_p find_free_cell_or_revoke_unused_cell(text_renderer_p renderer, text_renderer_font_p font, uint32_t glyph_width, uint32_t glyph_height, uint32_t texture_width, uint32_t texture_height, size_t* line_idx, size_t* cell_idx);

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
	
	for(size_t i = 0; i < renderer->lines->length; i++)
		array_destroy( array_elem(renderer->lines, text_renderer_line_t, i).cells );
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
	if (error) {
		printf("FT_New_Face error\n");
		goto failed_new_face;
	}
	
	error = FT_Set_Pixel_Sizes(font->face, 0, font_size);
	if (error) {
		printf("FT_Set_Pixel_Size error\n");
		goto failed_set_pixel_size;
	}
	
	font->cell_refs = hash_of(text_renderer_cell_ref_t);
	
	return handle;
	
	failed_set_pixel_size:
		FT_Done_Face(font->face);
	failed_new_face:
		hash_remove(renderer->fonts, handle);
	return -1;
}

void text_renderer_font_destroy(text_renderer_p renderer, int32_t font_handle) {
	text_renderer_font_p font = hash_get_ptr(renderer->fonts, font_handle);
	if (!font)
		return;
	
	// TODO: mark all cells of this font as free
	
	FT_Done_Face(font->face);
	hash_destroy(font->cell_refs);
	hash_remove(renderer->fonts, font_handle);
}


void text_renderer_prepare(text_renderer_p renderer, int32_t font_handle, uint32_t range_start, uint32_t range_end) {
	text_renderer_font_p font = hash_get_ptr(renderer->fonts, font_handle);
	if (!font)
		return;
	
	glBindTexture(GL_TEXTURE_RECTANGLE, renderer->texture);
	
	// Lookup the size of our texture (needed by find_free_cell_or_revoke_unused_cell() for some checks)
	GLint texture_width = 0, texture_height = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_WIDTH, &texture_width);
	glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_HEIGHT, &texture_height);
	
	GLint unpack_alignment = 0;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	
	for(uint32_t code_point = range_start; code_point <= range_end; code_point++) {
		// Handle line break.
		if (code_point == '\n') {
			continue;
		}
		
		// Look if this glyph is present in the texture. If so this font has a cell reference
		// for this code point.
		text_renderer_cell_ref_p cell_ref = hash_get_ptr(font->cell_refs, code_point);
		
		// Glyph not rendered yet, try to render it
		if (!cell_ref) {
			uint32_t glyph_index = FT_Get_Char_Index(font->face, code_point);
			if (glyph_index == 0)
				continue;
			
			FT_Error error = FT_Load_Glyph(font->face, glyph_index, FT_LOAD_RENDER);
			if (error)
				continue;
			
			// Look for a free cell to store the rendered glyph
			uint32_t gw = font->face->glyph->bitmap.width, gh = font->face->glyph->bitmap.rows;
			size_t line_idx = 0, cell_idx = 0;
			text_renderer_cell_p free_cell = find_free_cell_or_revoke_unused_cell(renderer, font, gw, gh, texture_width, texture_height, &line_idx, &cell_idx);
			
			if (free_cell == NULL)
				continue;
			
			free_cell->glyph_index    = glyph_index;
			free_cell->hori_bearing_x = font->face->glyph->metrics.horiBearingX / 64;
			free_cell->hori_bearing_y = font->face->glyph->metrics.horiBearingY / 64;
			free_cell->hori_advance   = font->face->glyph->metrics.horiAdvance  / 64;
			
			/*
			printf("%3u %c: %2ux%2u %3u bytes, pitch %2u, pos %3u/%3u hori_bearing: %2d/%2d, adv: %2d\n",
				code_point, code_point, gw, gh, gw*gh, font->face->glyph->bitmap.pitch,
				free_cell->x, free_cell->y, free_cell->hori_bearing_x, free_cell->hori_bearing_y, free_cell->hori_advance);
			*/
			
			glPixelStorei(GL_UNPACK_ROW_LENGTH, font->face->glyph->bitmap.pitch);
			glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, free_cell->x, free_cell->y, gw, gh, GL_RED, GL_UNSIGNED_BYTE, font->face->glyph->bitmap.buffer);
			
			cell_ref = hash_put_ptr(font->cell_refs, code_point);
			cell_ref->line_idx = line_idx;
			cell_ref->cell_idx = cell_idx;
		}
	}
	
	glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
}

size_t text_renderer_render(text_renderer_p renderer, int32_t font_handle, char* text, size_t x, size_t y, float* buffer_ptr, size_t buffer_size) {
	text_renderer_font_p font = hash_get_ptr(renderer->fonts, font_handle);
	if (!font)
		return 0;
	
	// The current position is the baseline of the text so start one line down
	// from the position. Otherwise we would render above it.
	size_t pos_x = x, pos_y = y + (font->face->size->metrics.height / 64);
	float* p = buffer_ptr;
	FT_UInt glyph_index = 0, prev_glyph_index = 0;
	
	glBindTexture(GL_TEXTURE_RECTANGLE, renderer->texture);
	
	// Lookup the size of our texture (needed by find_free_cell_or_revoke_unused_cell() for some checks)
	GLint texture_width = 0, texture_height = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_WIDTH, &texture_width);
	glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_HEIGHT, &texture_height);
	
	GLint unpack_alignment = 0;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	
	for(utf8_iterator_t it = utf8_first(text); it.code_point != 0; it = utf8_next(it)) {
		// Handle line break.
		if (it.code_point == '\n') {
			pos_y += font->face->size->metrics.height / 64;
			pos_x = x;
			
			// Don't do kerning at the next character after the line break.
			prev_glyph_index = 0;
			
			continue;
		}
		
		// Look if this glyph is present in the texture. If so this font has a cell reference
		// for this code point.
		text_renderer_cell_ref_p cell_ref = hash_get_ptr(font->cell_refs, it.code_point);
		
		// Glyph not rendered yet, try to render it
		if (!cell_ref) {
			glyph_index = FT_Get_Char_Index(font->face, it.code_point);
			if (glyph_index == 0)
				goto render_error_glyph;
			
			FT_Error error = FT_Load_Glyph(font->face, glyph_index, FT_LOAD_RENDER);
			if (error)
				goto render_error_glyph;
			
			// Look for a free cell to store the rendered glyph
			uint32_t gw = font->face->glyph->bitmap.width, gh = font->face->glyph->bitmap.rows;
			size_t line_idx = 0, cell_idx = 0;
			text_renderer_cell_p free_cell = find_free_cell_or_revoke_unused_cell(renderer, font, gw, gh, texture_width, texture_height, &line_idx, &cell_idx);
			
			if (free_cell == NULL)
				goto render_error_glyph;
			
			free_cell->glyph_index    = glyph_index;
			free_cell->hori_bearing_x = font->face->glyph->metrics.horiBearingX / 64;
			free_cell->hori_bearing_y = font->face->glyph->metrics.horiBearingY / 64;
			free_cell->hori_advance   = font->face->glyph->metrics.horiAdvance  / 64;
			
			/*
			printf("%c: %2ux%2u %2u bytes, pitch %u, x: %u, y: %u hori_bearing: %2u/%2u, adv: %2u\n",
				it.code_point, gw, gh, gw*gh, font->face->glyph->bitmap.pitch,
				free_cell->x, free_cell->y, free_cell->hori_bearing_x, free_cell->hori_bearing_y, free_cell->hori_advance);
			*/

			glPixelStorei(GL_UNPACK_ROW_LENGTH, font->face->glyph->bitmap.pitch);
			glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, free_cell->x, free_cell->y, gw, gh, GL_RED, GL_UNSIGNED_BYTE, font->face->glyph->bitmap.buffer);
			
			cell_ref = hash_put_ptr(font->cell_refs, it.code_point);
			cell_ref->line_idx = line_idx;
			cell_ref->cell_idx = cell_idx;
		}
		
		// Glyph rendered successfully, do kerning and generate vertices for it
		text_renderer_line_t line = array_elem(renderer->lines, text_renderer_line_t, cell_ref->line_idx);
		text_renderer_cell_t cell = array_elem(line.cells, text_renderer_cell_t, cell_ref->cell_idx);
		
		if (cell.glyph_index && prev_glyph_index) {
			FT_Vector delta;
			FT_Get_Kerning(font->face, prev_glyph_index, glyph_index, FT_KERNING_DEFAULT, &delta);
			pos_x += delta.x / 64;
		}
		
		// We have the texture coordinates of the glyph, generate the vertex buffer
		if ( (p + 6*4 - buffer_ptr) * sizeof(float) < buffer_size ) {
			float w = cell.width, h = line.height;
			
			float cx = pos_x + cell.hori_bearing_x;
			float cy = pos_y - cell.hori_bearing_y;
			
			float tl_x = cx + 0, tl_y = cy + 0,  tl_u = cell.x,     tl_v = cell.y;
			float tr_x = cx + w, tr_y = cy + 0,  tr_u = cell.x + w, tr_v = cell.y;
			float bl_x = cx + 0, bl_y = cy + h,  bl_u = cell.x,     bl_v = cell.y + h;
			float br_x = cx + w, br_y = cy + h,  br_u = cell.x + w, br_v = cell.y + h;
			
			*(p++) = tl_x; *(p++) = tl_y; *(p++) = tl_u; *(p++) = tl_v;
			*(p++) = tr_x; *(p++) = tr_y; *(p++) = tr_u; *(p++) = tr_v;
			*(p++) = bl_x; *(p++) = bl_y; *(p++) = bl_u; *(p++) = bl_v;
			*(p++) = tr_x; *(p++) = tr_y; *(p++) = tr_u; *(p++) = tr_v;
			*(p++) = br_x; *(p++) = br_y; *(p++) = br_u; *(p++) = br_v;
			*(p++) = bl_x; *(p++) = bl_y; *(p++) = bl_u; *(p++) = bl_v;
			
			pos_x += cell.hori_advance;
		}
		
		prev_glyph_index = glyph_index;
		continue;
		
		render_error_glyph:
			// For now just output nothing when we fail to render a glyph.
			// TODO: Figure out how to render a kind of error glyph.
			prev_glyph_index = glyph_index;
			continue;
	}
	
	glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	
	return (p - buffer_ptr) * sizeof(float);
}

static text_renderer_cell_p find_free_cell_or_revoke_unused_cell(text_renderer_p renderer, text_renderer_font_p font, uint32_t glyph_width, uint32_t glyph_height, uint32_t texture_width, uint32_t texture_height, size_t* line_idx, size_t* cell_idx) {
	// Padding between the glyphs
	const uint32_t padding = 1;
	// Keep track of the texture coordinates so we know where our cell is on the texture
	uint32_t x = 0, y = 0;
	
	// First search for a line with the matching height
	for(size_t i = 0; i < renderer->lines->length; i++) {
		text_renderer_line_p line = &array_data(renderer->lines, text_renderer_line_t)[i];
		
		if (line->height == glyph_height) {
			// We found a line with matching height, now look if we can get our cell in there
			x = 0;
			for(size_t j = 0; j < line->cells->length; j++) {
				text_renderer_cell_p current_cell = array_elem_ptr(line->cells, j);
				x += current_cell->width + padding;
			}
			
			if (x + glyph_width <= texture_width) {
				// There is space left at the end of this line, add our cell here
				text_renderer_cell_p cell = array_append_ptr(line->cells);
				memset(cell, 0, sizeof(text_renderer_cell_t));
				
				cell->x = x;
				cell->y = y;
				cell->width = glyph_width;
				
				*line_idx = i;
				*cell_idx = line->cells->length - 1;
				return cell;
			}
			
			// No space at end of line, search next line
		}
		
		y += line->height + padding;
	}
	
	// We haven't found a matching line or every matching line was full. Anyway add a new line
	// if there is space left at the bottom of the texture and put the cell in there.
	if (y + glyph_height + padding <= texture_height) {
		text_renderer_line_p line = array_append_ptr(renderer->lines);
		line->height = glyph_height;
		line->cells = array_of(text_renderer_cell_t);
		
		text_renderer_cell_p cell = array_append_ptr(line->cells);
		memset(cell, 0, sizeof(text_renderer_cell_t));
		
		cell->x = x;
		cell->y = y;
		cell->width = glyph_width;
		
		*line_idx = renderer->lines->length - 1;
		*cell_idx = 0;
		return cell;
	}
	
	// We would like to add a new line but there is not enough free space at the bottom
	// to the texture. For now just give up.
	*line_idx = (size_t)-1;
	*cell_idx = (size_t)-1;
	return NULL;
}