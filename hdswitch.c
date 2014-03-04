#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include <poll.h>

#include "drawable.h"
#include "stb_image.h"
#include "cam.h"
#include "sound.h"
#include "timer.h"


int main(int argc, char** argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s v4l2-file alsa-input\n", argv[0]);
		return 1;
	}
	
	int w = 800, h = 448;
	
	SDL_Init(SDL_INIT_VIDEO);
	atexit(SDL_Quit);
	
	SDL_Window* win = SDL_CreateWindow("HDSwitch", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
	//SDL_GL_SetSwapInterval(1);
	SDL_GL_SetSwapInterval(0);
	
	// Setup OpenGL stuff
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
	
	// Setup sound input and output
	size_t latency = 5;
	
	sound_p in = sound_open(argv[2], SOUND_CAPTURE, SOUND_NONBLOCK);
	sound_configure(in, 48000, 2, SOUND_FORMAT_S16, true, latency);
	
	sound_p out = sound_open("default", SOUND_PLAYBACK, SOUND_NONBLOCK);
	sound_configure(out, 48000, 2, SOUND_FORMAT_S16, true, latency);
	
	size_t buffer_size = in->buffer_size;
	size_t buffer_filled = 0;
	void* buffer = malloc(in->buffer_size);
	
	// Setup camera
	cam_p cam = cam_open(argv[1]);
	cam_print_info(cam);
	cam_setup(cam, __builtin_bswap32('YUYV'), w, h, 1, 30, NULL);
	cam_stream_start(cam, 2);
	
	
	size_t poll_fd_count = 1 + sound_poll_fds_count(in) + sound_poll_fds_count(out);
	struct pollfd pollfds[poll_fd_count];
	
	timeval_t mark;
	size_t overruns = 0, underruns = 0, flushes = 0;
	bool exit_mainloop = false;
	while (!exit_mainloop) {
		
		// Handle SLD events
		time_mark(&mark);
			SDL_Event event;
			while ( SDL_PollEvent(&event) ) {
				if (event.type == SDL_QUIT) {
					exit_mainloop = true;
					break;
				}
				
				if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED)
					glViewport(0, 0, event.window.data1, event.window.data2);
			}
		double sdl_time = time_mark(&mark);
		
		// Setup and do the poll
		time_mark(&mark);
			size_t poll_fds_used = 0;
			
			pollfds[0] = (struct pollfd){ .fd = cam->fd, .events = POLLIN, .revents = 0 };
			poll_fds_used++;
			
			struct pollfd* in_poll_fds = pollfds + poll_fds_used;
			size_t in_poll_fd_count = sound_poll_fds(in,  pollfds + poll_fds_used, poll_fd_count - poll_fds_used);
			poll_fds_used += in_poll_fd_count;
			
			struct pollfd* out_poll_fds = pollfds + poll_fds_used;
			size_t out_poll_fd_count = sound_poll_fds(out, pollfds + poll_fds_used, poll_fd_count - poll_fds_used);
			poll_fds_used += out_poll_fd_count;
			
			int active_fds = poll(pollfds, poll_fds_used, -1);
			if (active_fds == -1) {
				perror("poll");
				continue;
			}
		double poll_time = time_mark(&mark);
		
		
		double read_time = 0, write_time = 0;
		double cam_time = 0, tex_update_time = 0, draw_time = 0, swap_time = 0;
		ssize_t bytes_read = 0, bytes_written = 0;
		
		// Read and write pending audio data
		uint16_t in_revents  = sound_poll_fds_revents(in,  in_poll_fds,  in_poll_fd_count);
		uint16_t out_revents = sound_poll_fds_revents(out, out_poll_fds, out_poll_fd_count);
		
		if (in_revents & POLLIN) {
			mark = time_now();
			
			while( (bytes_read = sound_read(in, buffer + buffer_filled, buffer_size - buffer_filled)) < 0 ) {
				sound_recover(in, bytes_read);
				overruns++;
			}
			buffer_filled += bytes_read;
			
			read_time = time_mark(&mark);
		}
		
		if (out_revents & POLLOUT) {
			mark = time_now();
			
			while( (bytes_written = sound_write(out, buffer, buffer_filled)) < 0 ) {
				sound_recover(out, bytes_written);
				underruns++;
			}
			
			if (bytes_written < (ssize_t)buffer_filled) {
				// Samples still left in buffer, move to front of buffer.
				memmove(buffer, buffer + bytes_written, buffer_filled - bytes_written);
				buffer_filled = buffer_filled - bytes_written;
			} else {
				buffer_filled = 0;
			}
			
			write_time = time_mark(&mark);
		}
		
		// Flush the buffer if underruns cause the sample to queue up and create latency.
		if (buffer_filled > buffer_size / 2) {
			buffer_filled = 0;
			flushes++;
		}
		
		// Render new video frames
		if (pollfds[0].revents & POLLIN) {
			mark = time_now();
			
			cam_buffer_t frame = cam_frame_get(cam);
			cam_time = time_mark(&mark);
			
			texture_update(video->texture, GL_RG, frame.ptr);
			tex_update_time = time_mark(&mark);
			
			drawable_draw(video);
			draw_time = time_mark(&mark);
			
			SDL_GL_SwapWindow(win);
			swap_time = time_mark(&mark);
			
			cam_frame_release(cam);
		}
		
		// Output stats
		/*
		printf("poll: %zu fds, %d active, %4.1lf ms  ", poll_fds_used, active_fds, poll_time);
		printf("SDL: %4.1lf ms  ", sdl_time);
		printf("audio: %5zd bytes in, %4.1lf ms, %5zd bytes out, %4.1lf ms, %5zu buffer bytes, %4zu overruns, %4zu underruns, %4zu flushes  ",
			bytes_read, read_time, bytes_written, write_time, buffer_filled, overruns, underruns, flushes);
		printf("cam: %4.1lf ms, tex update: %4.1lf ms, draw: %4.1lf ms, swap: %4.1lf\r",
			cam_time, tex_update_time, draw_time, swap_time);
		*/
		//fflush(stdout);
		
		
		
		//printf("poll: %.1lf ms (%.1lf fps), cam: %.1lf ms, tex update: %.1lf ms, draw: %.1lf ms, swap: %.1lf\n",
		//	poll_time, 1000.0 / poll_time, frame_time, tex_update_time, draw_time, swap_time);
	}
	
	sound_close(in);
	sound_close(out);
	
	cam_stream_stop(cam);
	cam_close(cam);
	
	drawable_destroy(video);
	
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	return 0;
}