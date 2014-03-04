#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include <poll.h>

#include "drawable.h"
#include "stb_image.h"
#include "cam.h"
#include "timer.h"


int main(int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s v4l2-file\n", argv[0]);
		return 1;
	}
	
	int w = 800, h = 448;
	
	SDL_Init(SDL_INIT_VIDEO);
	atexit(SDL_Quit);
	
	SDL_Window* win = SDL_CreateWindow("HDSwitch", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
	//SDL_GL_SetSwapInterval(1);
	SDL_GL_SetSwapInterval(0);
	
	check_required_gl_extentions();
	
	//int x, y, n;
	//uint8_t* image_ptr = stbi_load("Haruhism - The Melancholy of Haruhi Suzumiya.jpg", &x, &y, &n, 3);
		drawable_p video = drawable_new(GL_TRIANGLE_STRIP, "shaders/video.vs", "shaders/video.fs");
		
		video->texture = texture_new(w, h, GL_RG8);
		//texture_update(video->texture, GL_RGB, image_ptr);
		
		// Triangle strip for a basic quad. Quads were removed in OpenGL 3.2.
		float tri_strip[] = {
			-1.0, -1.0,     0, h,
			-1.0,  1.0,     0, 0,
			 1.0, -1.0,     w, h,
			 1.0,  1.0,     w, 0
		};
		video->vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
	//free(image_ptr);
	
	
	cam_p cam = cam_open(argv[1]);
	cam_print_info(cam);
	cam_setup(cam, __builtin_bswap32('YUYV'), w, h, 1, 30, NULL);
	cam_stream_start(cam, 2);
	
	
	timeval_t mark = time_now();
	
	bool exit_mainloop = false;
	while (!exit_mainloop) {
		SDL_Event event;
		while ( SDL_PollEvent(&event) ) {
			if (event.type == SDL_QUIT) {
				exit_mainloop = true;
				break;
			}
			
			if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED)
				glViewport(0, 0, event.window.data1, event.window.data2);
		}
		
		time_mark(&mark);
			struct pollfd pollfds[1];
			pollfds[0] = (struct pollfd){ .fd = cam->fd, .events = POLLIN, .revents = 0 };
			
			int result = poll(pollfds, sizeof(pollfds) / sizeof(pollfds[0]), -1);
			if (result == -1)
				return perror("poll"), 1;
		double poll_time = time_mark(&mark);
		
		cam_buffer_t frame = cam_frame_get(cam);
		double frame_time = time_mark(&mark);
		//double ms = time_mark(&mark);
		//ms = ms;
		//printf("%.1lf ms, %.1lf fps, %zu bytes: %p\n", ms, 1000.0 / ms, frame.size, frame.ptr);
		
		texture_update(video->texture, GL_RG, frame.ptr);
		double tex_update_time = time_mark(&mark);
		drawable_draw(video);
		double draw_time = time_mark(&mark);
		SDL_GL_SwapWindow(win);
		double swap_time = time_mark(&mark);
		
		cam_frame_release(cam);
		
		printf("poll: %.1lf ms (%.1lf fps), cam: %.1lf ms, tex update: %.1lf ms, draw: %.1lf ms, swap: %.1lf\n",
			poll_time, 1000.0 / poll_time, frame_time, tex_update_time, draw_time, swap_time);
	}
	
	
	//while( SDL_WaitEvent(&event) ) {
		
		//glClearColor(0, 0, 0, 1);
		//glClear(GL_COLOR_BUFFER_BIT);
		
	//}
	
	
	cam_stream_stop(cam);
	cam_close(cam);
	drawable_destroy(video);
	
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	return 0;
}