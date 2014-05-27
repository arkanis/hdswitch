/**

Test commands for output:

ffplay unix:hdswitch.sock
ffmpeg -y -i unix:hdswitch.sock -f webm - | ffmpeg2theora --output /dev/stdout --format webm - | oggfwd -p -n event icecast.mi.hdm-stuttgart.de 80 xV2kxUG3 /test.ogv

*/

// For sigemptyset() and co. (need _POSIX_SOURCE)
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#include <pulse/pulseaudio.h>
#include <poll.h>

#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>

#include "drawable.h"
#include "stb_image.h"
#include "cam.h"
#include "timer.h"
#include "array.h"
#include "list.h"
#include "server.h"
#include "mixer.h"
#include "text_renderer.h"
#include "timer.h"


// Global stuff used by event callbacks
size_t scene_count = 0;
size_t scene_idx = 0;
size_t ww, wh;
bool something_to_render = false;
uint32_t start_ms = 0;

usec_t global_start_walltime = 0;

double video_upload_time = 0, sld_event_time = 0, dispatch_time = 0, mixer_output_time = 0;
double compose_time = 0, colorspace_time = 0, video_download_time = 0, enqueue_video_frame_time = 0;
double draw_video_time = 0, draw_text_time = 0;
double total_time = 0;

double total_time_max = 0, total_time_avg = 0, total_time_avg_sum = 0;
uint32_t total_time_count = 0;


static int signals_init();
static int signals_cleanup(int signal_fd);

static void signals_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
static void sdl_event_check_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata);
static void camera_frame_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);


typedef struct {
	char* device_file;
	size_t w, h;
	cam_p cam;
	GLuint tex;
} video_input_t, *video_input_p;

typedef struct {
	char    horizontal_anchor;
	ssize_t x;
	char    vertical_anchor;
	ssize_t y;
	
	size_t video_idx;
	ssize_t w, h;
	
	GLuint  vertices;
} video_view_t, *video_view_p;


int main(int argc, char** argv) {
	
	// One cam test setup
	video_input_t video_inputs[] = {
		{ "/dev/video0", 640, 480, NULL, 0 }
	};
	
	video_view_t *scenes[] = {
		(video_view_t[]){
			{ 'l', 0, 'c', 0,     0, -100, 0,     0 },
			{ 0 }
		},
		(video_view_t[]){
			{ 'l', 0, 'c', 0,     0, -100, 0,     0 },
			{ 'r', 0, 'b', 0,     0,  -33, 0,     0 },
			{ 0 }
		}
	};
	/*
	// Two cam setup
	video_input_t video_inputs[] = {
		{ "/dev/video0", 640, 480, NULL, 0 },
		{ "/dev/video1", 640, 480, NULL, 0 }
	};
	
	video_view_t *scenes[] = {
		(video_view_t[]){
			{ 'l', 0, 'c', 0,     0, -100, 0,     0 },
			{ 0 }
		},
		(video_view_t[]){
			{ 'l', 0, 'c', 0,     0, -100, 0,     0 },
			{ 'r', 0, 'b', 0,     1,  -33, 0,     0 },
			{ 0 }
		},
		(video_view_t[]){
			{ 'c', 0, 'c', 0,     1, -100, 0,     0 },
			{ 0 }
		}
	};
	*/
	
	// Calculate rest of the configuration
	size_t video_input_count = sizeof(video_inputs) / sizeof(video_inputs[0]);
	scene_count = sizeof(scenes) / sizeof(scenes[0]);
	
	// Composite (output video) size
	size_t composite_w = 0, composite_h = 0;
	for(size_t i = 0; i < video_input_count; i++) {
		composite_w = (composite_w > video_inputs[i].w) ? composite_w : video_inputs[i].w;
		composite_h = (composite_h > video_inputs[i].h) ? composite_h : video_inputs[i].h;
	}
	
	// Sizes of individual video views
	for(size_t i = 0; i < scene_count; i++) {
		for(size_t j = 0; scenes[i][j].horizontal_anchor != 0; j++) {
			video_view_p  vv = &scenes[i][j];
			video_input_p vi = &video_inputs[vv->video_idx];
			
			// percent size to pixel size
			if (vv->w < 0) vv->w = (ssize_t)vi->w * vv->w / -100;
			if (vv->h < 0) vv->h = (ssize_t)vi->h * vv->h / -100;
			
			// calculate aspect ratio correct height if height is 0
			if (vv->h == 0) vv->h = vv->w * (ssize_t)vi->h / (ssize_t)vi->w;
			
			// position (with anchor)
			switch (vv->horizontal_anchor) {
				case 'l': /* nothing to do, x already correct*/    break;
				case 'r': vv->x = composite_w - vv->x - vv->w;     break;
				case 'c': vv->x = (composite_w - vv->w) / 2 + vv->x; break;
			}
			
			switch (vv->vertical_anchor) {
				case 't': /* nothing to do, y already correct*/    break;
				case 'b': vv->y = composite_h - vv->y - vv->h;     break;
				case 'c': vv->y = (composite_h - vv->h) / 2 + vv->y; break;
			}
		}
	}
	
	
	// Init mainloop
	pa_mainloop* poll_mainloop = pa_mainloop_new();
	pa_mainloop_api* mainloop = pa_mainloop_get_api(poll_mainloop);
	
	// Init signal handling
	int signal_fd = signals_init();
	
	// Init SDL
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
	atexit(SDL_Quit);
	
	ww = composite_w;
	wh = composite_h;
	SDL_Window* win = SDL_CreateWindow("HDSwitch", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
	SDL_GL_SetSwapInterval(0);
	
	
	// Setup OpenGL stuff
	check_required_gl_extentions();
	
	
	// Setup videos and textures
	for(size_t i = 0; i < video_input_count; i++) {
		video_input_p vi = &video_inputs[i];
		
		vi->cam = cam_open(vi->device_file);
		cam_print_info(vi->cam);
		cam_setup(vi->cam, cam_pixel_format('YUYV'), vi->w, vi->h, 30, 1, NULL);
		cam_print_frame_rate(vi->cam);
		
		vi->tex = texture_new(vi->w, vi->h, GL_RG8);
	}
	
	GLuint composite_video_tex  = texture_new(composite_w, composite_h, GL_RGB8);
	GLuint stream_video_tex     = texture_new(composite_w, composite_h, GL_RG8);
	size_t stream_video_size = composite_w * composite_h * 2;
	void*  stream_video_ptr  = malloc(stream_video_size);
	
	
	// Initialize the textures so we don't get random GPU RAM garbage in
	// our first composite frame when one webcam frame hasn't been uploaded yet
	GLuint clear_fbo = 0;
	glGenFramebuffers(1, &clear_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, clear_fbo);
		// Clear rest to black (luma 0, cb 0, cr 0 but croma channels are not in [-0.5, 0.5] but in [0, 1] so 0.5 it is)
		glClearColor(0, 0.5, 0, 0);
		
		for(size_t i = 0; i < video_input_count; i++) {
			video_input_p vi = &video_inputs[i];
			
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, vi->tex, 0);
			if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				return fprintf(stderr, "Framebuffer setup failed to clear video texture\n"), 1;
			glViewport(0, 0, vi->w, vi->h);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &clear_fbo);
	
	fbo_p composite_video = fbo_new(composite_video_tex);
	
	// Build vertex buffers, use triangle strips for a basic quad. Quads were removed in OpenGL 3.2.
	drawable_p video_on_composite = drawable_new(GL_TRIANGLE_STRIP, "shaders/video_on_composite.vs", "shaders/video_on_composite.fs");
	size_t cw = composite_w, ch = composite_h;
	for(size_t i = 0; i < scene_count; i++) {
		for(size_t j = 0; scenes[i][j].horizontal_anchor != 0; j++) {
			video_view_p  vv = &scenes[i][j];
			video_input_p vi = &video_inputs[vv->video_idx];
			
			// B D
			// A C
			float tri_strip[] = {
				// pos x                           y                                  tex coord u, v
				 vv->x          / (cw / 2.0f) - 1, (vv->y + vv->h) / (ch / 2.0f) - 1,         0, vi->h,
				 vv->x          / (cw / 2.0f) - 1,  vv->y          / (ch / 2.0f) - 1,         0,     0,
				(vv->x + vv->w) / (cw / 2.0f) - 1, (vv->y + vv->h) / (ch / 2.0f) - 1,     vi->w, vi->h,
				(vv->x + vv->w) / (cw / 2.0f) - 1,  vv->y          / (ch / 2.0f) - 1,     vi->w,     0
			};
			vv->vertices = buffer_new(sizeof(tri_strip), tri_strip);
		}
	}
	
	drawable_p gui = drawable_new(GL_TRIANGLE_STRIP, "shaders/video.vs", "shaders/video.fs");
	gui->texture = composite_video_tex;
	{
		float tri_strip[] = {
			-1.0, -1.0,      0, ch,
			-1.0,  1.0,      0,  0,
			 1.0, -1.0,     cw, ch,
			 1.0,  1.0,     cw,  0
		};
		gui->vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
	}
	
	fbo_p stream_fbo = fbo_new(stream_video_tex);
	drawable_p stream = drawable_new(GL_TRIANGLE_STRIP, "shaders/composite_on_stream.vs", "shaders/composite_on_stream.fs");
	stream->texture = composite_video_tex;
	{
		float tri_strip[] = {
			-1.0, -1.0,      0,  0,
			-1.0,  1.0,      0, ch,
			 1.0, -1.0,     cw,  0,
			 1.0,  1.0,     cw, ch
		};
		stream->vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
	}
	
	// Init GUI and text rendering
	text_renderer_t tr;
	text_renderer_new(&tr, 512, 512);
	uint32_t status_font = text_renderer_font_new(&tr, "DroidSans.ttf", 14);
	text_renderer_prepare(&tr, status_font, 0, 127);
	
	drawable_p text = drawable_new(GL_TRIANGLES, "shaders/text.vs", "shaders/text.fs");
	text->vertex_buffer = buffer_new(0, NULL);
	text->texture = tr.texture;
	float text_vertex_buffer[6*4*400];
	
	// Setup sound input and output
	pa_sample_spec mixer_sample_spec = {
		.format   = PA_SAMPLE_S16LE,
		.rate     = 48000,
		.channels = 2
	};
	
	
	// Init local server
	server_start("hdswitch.sock", cw, ch, mixer_sample_spec.rate, mixer_sample_spec.channels, pa_sample_size(&mixer_sample_spec) * 8, mainloop);
	
	
	// Start everything up
	for(size_t i = 0; i < video_input_count; i++)
		cam_stream_start(video_inputs[i].cam, 2);
	
	
	// Init sound
	global_start_walltime = time_now();
	mixer_start(global_start_walltime, 10, 1000, 30, mixer_sample_spec, mainloop);
	
	// Prepare mainloop event callbacks
	mainloop->io_new(mainloop, signal_fd, PA_IO_EVENT_INPUT, signals_cb, NULL);
	
	struct timeval next_sdl_check_time = usec_to_timeval( time_now() + 25000 );
	mainloop->time_new(mainloop, &next_sdl_check_time, sdl_event_check_cb, NULL);
	
	for(size_t i = 0; i < video_input_count; i++) {
		video_input_p vi = &video_inputs[i];
		pa_io_event* e = mainloop->io_new(mainloop, vi->cam->fd, PA_IO_EVENT_INPUT, camera_frame_cb, vi);
		//mainloop->io_enable(e, PA_IO_EVENT_NULL);
	}
	
	
	// Do the loop
	usec_t performance_timer = time_now();
	usec_t total_timer = time_now();
	while ( pa_mainloop_iterate(poll_mainloop, true, NULL) >= 0 ) {
		dispatch_time = time_mark_ms(&performance_timer);
		
		// Check for any audio data from the mixer we need to give to the server
		void* buffer_ptr = NULL;
		size_t buffer_size = 0;
		uint64_t buffer_pts = 0;
		mixer_output_peek(&buffer_ptr, &buffer_size, &buffer_pts);
		if (buffer_size > 0) {
			server_enqueue_frame(2, buffer_pts, buffer_ptr, buffer_size);
			mixer_output_consume();
		}
		mixer_output_time = time_mark_ms(&performance_timer);
		
		// Render new video frames if one or more frames have been uploaded
		if (something_to_render) {
			video_view_p scene = scenes[scene_idx];
			
			fbo_bind(composite_video);
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT);
				
				for(video_view_p vv = scene; vv->horizontal_anchor != 0; vv++) {
					video_input_p vi = &video_inputs[vv->video_idx];
					
					video_on_composite->texture = vi->tex;
					video_on_composite->vertex_buffer = vv->vertices;
					drawable_draw(video_on_composite);
				}
				
			fbo_bind(stream_fbo);
				compose_time = time_mark_ms(&performance_timer);
				
				drawable_draw(stream);
				
			fbo_bind(NULL);
			colorspace_time = time_mark_ms(&performance_timer);
			
			fbo_read(stream_fbo, GL_RG, GL_UNSIGNED_BYTE, stream_video_ptr);
			video_download_time = time_mark_ms(&performance_timer);
			
			uint64_t timecode = time_now() - global_start_walltime;
			server_enqueue_frame(1, timecode, stream_video_ptr, stream_video_size);
			enqueue_video_frame_time = time_mark_ms(&performance_timer);
			
			glViewport(0, 0, ww, wh);
			
			drawable_draw(gui);
			draw_video_time = time_mark_ms(&performance_timer);
			
			char text_buffer[512];
			snprintf(text_buffer, sizeof(text_buffer), "event dispatch: %.2lf ms, sdl: %.2lf ms, video upload: %.2lf\ncompose: %.2lf colorspace: %.2lf ms download: %.2lf ms enqueue: %.2lf ms\ndraw video: %.2lf ms text: %.2lf ms\ntotal: %.2lf ms, avg %.2lf ms, max %.2lf ms",
				dispatch_time, sld_event_time, video_upload_time,
				compose_time, colorspace_time, video_download_time, enqueue_video_frame_time,
				draw_video_time, draw_text_time,
				total_time, total_time_avg, total_time_max);
			size_t buffer_used = text_renderer_render(&tr, status_font, text_buffer, 10, 10, text_vertex_buffer, sizeof(text_vertex_buffer));
			buffer_update(text->vertex_buffer, buffer_used, text_vertex_buffer, GL_STREAM_DRAW);
			
			glEnable(GL_BLEND);
				glBlendEquation(GL_FUNC_ADD);
				glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
				
				drawable_begin_uniforms(text);
					float screen_to_normal[9] = {
						2.0 / ww,  0,        -1,
						0,        -2.0 / wh,  1,
						0,         0,         1
					};
					glUniformMatrix3fv( glGetUniformLocation(text->program, "screen_to_normal"), 1, true, screen_to_normal );
				drawable_draw(text);
			glDisable(GL_BLEND);
			draw_text_time = time_mark_ms(&performance_timer);
			
			SDL_GL_SwapWindow(win);
			
			something_to_render = false;
		}
		
		total_time = time_mark_ms(&total_timer);
		
		if (total_time_count < 100) {
			total_time_avg_sum += total_time;
			total_time_count++;
		} else {
			total_time_max = 0;
			total_time_avg = total_time_avg_sum / total_time_count;
			total_time_avg_sum = 0;
			total_time_count = 0;
		}
		
		if (total_time > total_time_max)
			total_time_max = total_time;
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
	
	server_stop();
	mixer_stop();
	
	for(size_t i = 0; i < video_input_count; i++) {
		video_input_p vi = &video_inputs[i];
		
		cam_stream_stop(vi->cam);
		cam_close(vi->cam);
		
		texture_destroy(vi->tex);
	}
	
	for(size_t i = 0; i < scene_count; i++) {
		for(size_t j = 0; scenes[i][j].horizontal_anchor != 0; j++) {
			video_view_p  vv = &scenes[i][j];
			buffer_destroy(vv->vertices);
		}
	}
	
	text_renderer_destroy(&tr);
	drawable_destroy(text);
	
	drawable_destroy(stream);
	drawable_destroy(gui);
	drawable_destroy(video_on_composite);
	
	fbo_destroy(stream_fbo);
	fbo_destroy(composite_video);
	texture_destroy(composite_video_tex);
	free(stream_video_ptr);
	
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	pa_mainloop_free(poll_mainloop);
	signals_cleanup(signal_fd);
	
	return 0;
}



//
// Signal handling
//

static int signals_init() {
	// Ignore SIGPIPE. This happens when we write into a closed socket. In that case the write()
	// function also returns EPIPE which makes it easy to handle locally. No need for a signal here.
	if ( signal(SIGPIPE, SIG_IGN) == SIG_ERR )
		return perror("signal"), -1;
	
	// Setup SIGINT and SIGTERM to terminate our poll loop. For that we read them via a signal fd.
	// To prevent the signals from interrupting our process we need to block them first.
	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGINT);
	sigaddset(&signal_mask, SIGTERM);
	
	if ( sigprocmask(SIG_BLOCK, &signal_mask, NULL) == -1 )
		return perror("sigprocmask"), -1;
	
	int signals = signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (signals == -1) {
		perror("signalfd");
		
		if ( sigprocmask(SIG_UNBLOCK, &signal_mask, NULL) == -1 )
			perror("sigprocmask");
		
		return -1;
	}
	
	return signals;
}

static int signals_cleanup(int signal_fd) {
	close(signal_fd);
	
	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGINT);
	sigaddset(&signal_mask, SIGTERM);
	if ( sigprocmask(SIG_UNBLOCK, &signal_mask, NULL) == -1 )
		return perror("sigprocmask"), -1;
	
	if ( signal(SIGPIPE, SIG_DFL) == SIG_ERR )
		return perror("signal"), -1;
	
	return 1;
}



//
// Event handling callbacks.
//

// Called when a signal is received
static void signals_cb(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	struct signalfd_siginfo siginfo = {0};
	
	if ( read(fd, &siginfo, sizeof(siginfo)) == -1 ) {
		perror("read(signalfd)");
		return;
	}
	
	mainloop->quit(mainloop, 0);
}

// Called periodically to handle pending SDL events
static void sdl_event_check_cb(pa_mainloop_api *mainloop, pa_time_event *e, const struct timeval *tv, void *userdata) {
	usec_t start = time_now();
	
	SDL_Event event;
	while ( SDL_PollEvent(&event) ) {
		if (event.type == SDL_QUIT) {
			mainloop->quit(mainloop, 0);
			break;
		}
		
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
			ww = event.window.data1;
			wh = event.window.data2;
		}
		
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym >= SDLK_1 && event.key.keysym.sym <= SDLK_9) {
			size_t num = event.key.keysym.sym - SDLK_1;
			if (num < scene_count) {
				printf("switching to scene %zu\n", num);
				scene_idx = num;
			}
		}
		
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym >= SDLK_d) {
			server_flush_and_disconnect_clients();
		}
	}
	
	// Restart the timer for the next time
	struct timeval next_sdl_check_time = usec_to_timeval( time_now() + 25000 );
	mainloop->time_restart(e, &next_sdl_check_time);
	
	sld_event_time = time_mark_ms(&start);
}

// Upload new video frames to the GPU
static void camera_frame_cb(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	video_input_p video_input = userdata;
	
	usec_t start = time_now();
		cam_buffer_t frame = cam_frame_get(video_input->cam);
			texture_update(video_input->tex, GL_RG, frame.ptr);
		cam_frame_release(video_input->cam);
	video_upload_time = time_mark_ms(&start);
	
	something_to_render = true;
}