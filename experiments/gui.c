#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include "../drawable.h"
#include "../stb_image.h"
#include "../text_renderer.h"
#include "../tree.h"


typedef struct {
	float x, y, w, h;
	float r, g, b;
} elem_t, *elem_p;

int main() {
	tree_p elem_tree = tree_new();
	
	tree_p n1 = tree_append(elem_tree, elem_t,     ((elem_t){ .x = 100, .y = 100, .w = 200, .h = 200,    .r = 0.5, .g = 0.5, .b = 0.5 }));
		tree_p n13 = tree_append(n1, elem_t,       ((elem_t){ .x = 0,   .y = 0,   .w = 50,  .h = 50,     .r = 0.0, .g = 0.0, .b = 0.0 }));
			              tree_append(n13, elem_t, ((elem_t){ .x = 100, .y = 100, .w = 50,  .h = 50,     .r = 0.5, .g = 0.0, .b = 0.0 }));
	           tree_append(elem_tree, elem_t,      ((elem_t){ .x = 100, .y = 400, .w = 200, .h = 200,    .r = 0.0, .g = 0.0, .b = 0.75 }));
	
	
	SDL_Init(SDL_INIT_VIDEO);
	atexit(SDL_Quit);
	
	int org_win_w = 800, org_win_h = 600;
	SDL_Window* win = SDL_CreateWindow("HDSwitch GUI experiment", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, org_win_w, org_win_h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
	SDL_GL_SetSwapInterval(1);
	//glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	
	// Setup OpenGL stuff
	check_required_gl_extentions();
	
	// Generate vertex buffer for GUI
	float buffer[6*5*20];
	float *p = buffer, *buffer_end = buffer + (sizeof(buffer) / sizeof(buffer[0]));
	for(tree_p n = tree_first_pre(elem_tree); n != NULL; n = tree_next_pre(elem_tree, n)) {
		if (p + 6*5 >= buffer_end)
			break;
		
		float gx = 0, gy = 0;
		tree_p parent = n->parent;
		while(parent) {
			elem_p e = tree_value_ptr(parent);
			gx += e->x;
			gy += e->y;
			parent = parent->parent;
		}
		
		elem_p e = tree_value_ptr(n);
		float x = gx + e->x, y = gy + e->y, w = e->w, h = e->h;
		
		float tl_x = x + 0, tl_y = y + 0;
		float tr_x = x + w, tr_y = y + 0;
		float bl_x = x + 0, bl_y = y + h;
		float br_x = x + w, br_y = y + h;
		
		*(p++) = tl_x; *(p++) = tl_y; *(p++) = e->r; *(p++) = e->g; *(p++) = e->b;
		*(p++) = tr_x; *(p++) = tr_y; *(p++) = e->r; *(p++) = e->g; *(p++) = e->b;
		*(p++) = bl_x; *(p++) = bl_y; *(p++) = e->r; *(p++) = e->g; *(p++) = e->b;
		
		*(p++) = tr_x; *(p++) = tr_y; *(p++) = e->r; *(p++) = e->g; *(p++) = e->b;
		*(p++) = br_x; *(p++) = br_y; *(p++) = e->r; *(p++) = e->g; *(p++) = e->b;
		*(p++) = bl_x; *(p++) = bl_y; *(p++) = e->r; *(p++) = e->g; *(p++) = e->b;
	}
	size_t buffer_used = (p - buffer) * sizeof(float);
	
	
	drawable_p gui_rects = drawable_new(GL_TRIANGLES, "gui_rect.vs", "gui_rect.fs");
	gui_rects->vertex_buffer = buffer_new(buffer_used, buffer);		
	
	SDL_Event event;
	int win_w = org_win_w, win_h = org_win_h;
	while ( SDL_WaitEvent(&event) ) {
		if (event.type == SDL_QUIT)
			break;
		
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
			win_w = event.window.data1;
			win_h = event.window.data2;
			glViewport(0, 0, win_w, win_h);
		}
		
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r) {
			SDL_SetWindowSize(win, org_win_w, org_win_h);
			win_w = org_win_w;
			win_h = org_win_h;
			glViewport(0, 0, win_w, win_h);
		}
		
		glClearColor(1, 1, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		
		glUseProgram(gui_rects->program);
			float screen_to_normal[9] = {
				2.0 / win_w,  0,           -1,
				0,           -2.0 / win_h,  1,
				0,            0,            1
			};
			glUniformMatrix3fv( glGetUniformLocation(gui_rects->program, "screen_to_normal"), 1, true, screen_to_normal );
			
			// Setup vertex data association for the draw call
			GLint pos_attrib = glGetAttribLocation(gui_rects->program, "pos");
			GLint color_attrib = glGetAttribLocation(gui_rects->program, "color");
			if (pos_attrib == -1 || color_attrib == -1) {
				fprintf(stderr, "Can't draw, program doesn't have the \"pos\" or \"color\" attribute!\n");
				exit(1);
			}
			
			glBindBuffer(GL_ARRAY_BUFFER, gui_rects->vertex_buffer);
				
				size_t vertex_size = sizeof(float) * 5;
				GLint vertex_buffer_size = 0;
				glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vertex_buffer_size);
				
				glEnableVertexAttribArray(pos_attrib);
				glVertexAttribPointer(pos_attrib, 2, GL_FLOAT, GL_FALSE, vertex_size, 0);
				glEnableVertexAttribArray(color_attrib);
				glVertexAttribPointer(color_attrib, 3, GL_FLOAT, GL_FALSE, vertex_size, (void*)(2 * sizeof(float)));
				
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			
			GLenum error = glGetError();
			if (error != GL_NO_ERROR) {
				fprintf(stderr, "Vertex setup failed!\n");
				exit(1);
			}
			
			// Draw the vertecies
			glDrawArrays(gui_rects->primitive_type, 0, vertex_buffer_size / vertex_size);
			if (glGetError() != GL_NO_ERROR) {
				fprintf(stderr, "Draw failed!\n");
				exit(1);
			}
			
			glDisableVertexAttribArray(color_attrib);
			glDisableVertexAttribArray(pos_attrib);
		glUseProgram(0);
		
		SDL_GL_SwapWindow(win);
	}
	
	drawable_destroy(gui_rects);
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	tree_destroy(elem_tree);
	
	return 0;
}