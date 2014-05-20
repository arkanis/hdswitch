#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include "../drawable.h"
#include "../stb_image.h"
#include "../text_renderer.h"



int main(int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s image\n", argv[0]);
		return 1;
	}
	
	SDL_Init(SDL_INIT_VIDEO);
	atexit(SDL_Quit);
	
	int w, h, n;
	uint8_t* image_ptr = stbi_load(argv[1], &w, &h, &n, 3);
		SDL_Window* win = SDL_CreateWindow("HDSwitch", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
		SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
		SDL_GL_SetSwapInterval(1);
		
		// Setup OpenGL stuff
		check_required_gl_extentions();
		
		drawable_p image = drawable_new(GL_TRIANGLE_STRIP, "fbo.vs", "fbo.fs");
		
		image->texture = texture_new(w, h, GL_RGB8);
		texture_update(image->texture, GL_RGB, image_ptr);
		
		// Triangle strip for a basic quad
		float tri_strip[] = {
			-1.0, -1.0,     0,     h / 1,
			-1.0,  1.0,     0,     0,
			 1.0, -1.0,     w / 1, h / 1,
			 1.0,  1.0,     w / 1, 0
		};
		image->vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);		
	free(image_ptr);
	
	text_renderer_t tr;
	text_renderer_new(&tr, 512, 512);
	float buffer[6*4*200];
	
	uint32_t droid_sans = text_renderer_font_new(&tr, "DroidSans.ttf", 14);
	uint32_t headline = text_renderer_font_new(&tr, "DroidSans.ttf", 40);
	
	text_renderer_prepare(&tr, droid_sans, 0, 255);
	size_t bytes_used = 0;
	bytes_used += text_renderer_render(&tr, droid_sans, "Hello Text Rendering!\nNext line.", 0, 0, buffer + (bytes_used / sizeof(float)), sizeof(buffer) - bytes_used);
	bytes_used += text_renderer_render(&tr, headline, "Text Rendering!", 0, 150, buffer + (bytes_used / sizeof(float)), sizeof(buffer) - bytes_used);
	
	
	drawable_p text = drawable_new(GL_TRIANGLES, "text.vs", "text.fs");
	text->texture = tr.texture;
	text->vertex_buffer = buffer_new(bytes_used, buffer);
	
	//image->texture = text->texture;
	
	SDL_Event event;
	int win_w = w, win_h = h;
	while ( SDL_WaitEvent(&event) ) {
		if (event.type == SDL_QUIT)
			break;
		
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
			win_w = event.window.data1;
			win_h = event.window.data2;
			glViewport(0, 0, win_w, win_h);
		}
		
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r) {
			SDL_SetWindowSize(win, w, h);
			win_w = w;
			win_h = h;
			glViewport(0, 0, win_w, win_h);
		}
		
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		drawable_draw(image);
		
		glEnable(GL_BLEND);
		
		glBlendEquation(GL_FUNC_ADD);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		
		drawable_begin_uniforms(text);
			float screen_to_normal[9] = {
				2.0 / win_w,  0,           -1,
				0,           -2.0 / win_h,  1,
				0,            0,            1
			};
			glUniformMatrix3fv( glGetUniformLocation(text->program, "screen_to_normal"), 1, true, screen_to_normal );
		drawable_draw(text);
		glDisable(GL_BLEND);
		
		SDL_GL_SwapWindow(win);
	}
	
	text_renderer_font_destroy(&tr, droid_sans);
	text_renderer_destroy(&tr);
	drawable_destroy(text);
	drawable_destroy(image);
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	return 0;
}