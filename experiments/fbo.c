#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include "../drawable.h"
#include "../stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


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
		
		drawable_p video = drawable_new(GL_TRIANGLE_STRIP, "fbo.vs", "fbo.fs");
		
		video->texture = texture_new(w, h, GL_RGB8);
		texture_update(video->texture, GL_RGB, image_ptr);
		
		// Triangle strip for a basic quad. Quads were removed in OpenGL 3.2.
		float tri_strip[] = {
			-1.0, -1.0,     0, 0,
			-1.0,  1.0,     0, h,
			 1.0, -1.0,     w, 0,
			 1.0,  1.0,     w, h
		};
		video->vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
	free(image_ptr);
	
	drawable_p gui = drawable_new(GL_TRIANGLE_STRIP, "fbo.vs", "fbo.fs");
	float tri_strip2[] = {
		-1.0, -1.0,     0, h,
		-1.0,  1.0,     0, 0,
		 1.0, -1.0,     w, h,
		 1.0,  1.0,     w, 0
	};
	gui->vertex_buffer = buffer_new(sizeof(tri_strip2), tri_strip2);
	
	GLuint fbo_tex = 0;
	glGenTextures(1, &fbo_tex);
	glBindTexture(GL_TEXTURE_RECTANGLE, fbo_tex);
	glTexStorage2D(GL_TEXTURE_RECTANGLE, 1, GL_RGB8, w, h);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	gui->texture = fbo_tex;
	
	GLuint fb = 0;
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
	glBindTexture(GL_TEXTURE_RECTANGLE, fbo_tex);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, fbo_tex, 0);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	
	if ( glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE )
		return fprintf(stderr, "framebuffer setup failed\n"), 1;
	
	SDL_Event event;
	int win_w = w, win_h = h;
	while ( SDL_WaitEvent(&event) ) {
		if (event.type == SDL_QUIT)
			break;
		
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
			win_w = event.window.data1;
			win_h = event.window.data2;
		}
		
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r) {
			SDL_SetWindowSize(win, w, h);
			win_w = w;
			win_h = h;
		}
		
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
			glViewport(0, 0, w, h);
			drawable_draw(video);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_s) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
				size_t image_size = w * h * 3;
				void*  image_ptr  = malloc(image_size);
				
				glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, image_ptr);
				stbi_write_png("test.png", w, h, 3, image_ptr, 0);
				
				free(image_ptr);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		}
		
		glViewport(0, 0, win_w, win_h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		drawable_draw(gui);
		SDL_GL_SwapWindow(win);
	}
	
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fb);
	glDeleteTextures(1, &fbo_tex);
	
	drawable_destroy(video);
	drawable_destroy(gui);
	
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	return 0;
}