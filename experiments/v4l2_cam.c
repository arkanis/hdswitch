#include <stdio.h>
#include <math.h>

#include <SDL/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include "../drawable.h"
#include "../cam.h"

int main(int argc, char** argv) {
	if (argc != 5) {
		fprintf(stderr, "usage: %s v4l2-file width height fps\n", argv[0]);
		return 1;
	}
	
	size_t ww = 800,           wh = 450;
	size_t vw = atoi(argv[2]), vh = atoi(argv[3]);
	size_t fps = atoi(argv[4]);
	float  mx = 0, my = 0;
	
	// SDL and OpenGL stuff
	SDL_Init(SDL_INIT_VIDEO);
	atexit(SDL_Quit);
	
	SDL_Window* win = SDL_CreateWindow("Video4Linux test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
	SDL_GL_SetSwapInterval(0);
	check_required_gl_extentions();
	
	drawable_p video = drawable_new(GL_TRIANGLE_STRIP, "v4l2_cam.vs", "v4l2_cam.fs");
	video->texture = texture_new(vw, vh, GL_RG8);
	// Triangle strip for a basic quad. Quads were removed in OpenGL 3.2.
	float tri_strip[] = {
		-1.0, -1.0,     0,  vh,
		-1.0,  1.0,     0,  0,
		 1.0, -1.0,     vw, vh,
		 1.0,  1.0,     vw, 0
	};
	video->vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
	
	glViewport(0, 0, ww, wh);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	
	// Camera stuff
	cam_p cam = cam_open(argv[1]);
	cam_print_info(cam);
	cam_print_frame_rate(cam);
	cam_setup(cam, __builtin_bswap32('YUYV'), vw, vh, fps, 1, NULL);
	cam_print_frame_rate(cam);
	
	if ( ! cam_stream_start(cam, 3) )
		return 1;
	
	bool running = true;
	while(running) {
		SDL_Event event;
		while ( SDL_PollEvent(&event) ) {
			if (event.type == SDL_QUIT) {
				running = false;
				break;
			}
			
			if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
				ww = event.window.data1;
				wh = event.window.data2;
				glViewport(0, 0, ww, wh);
			}
			
			if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r) {
				SDL_SetWindowSize(win, 800, 450);
				ww = 800;
				wh = 450;
			}
			
			if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_o) {
				float w = fminf(ww, vw), h = fminf(wh, vh);
				float tri_strip[] = {
					-1.0, -1.0,     0, h,
					-1.0,  1.0,     0, 0,
					 1.0, -1.0,     w, h,
					 1.0,  1.0,     w, 0
				};
				buffer_update(video->vertex_buffer, sizeof(tri_strip), tri_strip, GL_STATIC_DRAW);
			}
			
			if (event.type == SDL_MOUSEMOTION) {
				mx = (float)event.motion.x / ww * vw;
				my = (float)event.motion.y / wh * vh;
			}
		}
		
		cam_buffer_t frame = cam_frame_get(cam);
		//printf("%zu bytes: %p\n", frame.size, frame.ptr);
		texture_update(video->texture, GL_RG, frame.ptr);
		cam_frame_release(cam);
		
		drawable_begin_uniforms(video);
		glUniform2f(glGetUniformLocation(video->program, "mouse_pos"), mx, my);
		drawable_draw(video);
		SDL_GL_SwapWindow(win);
	}
	cam_stream_stop(cam);
	
	cam_close(cam);
	drawable_destroy(video);
	
	return 0;
}