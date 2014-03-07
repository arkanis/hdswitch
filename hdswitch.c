/**

Test commands for output:

ffplay unix:///home/steven/projects/events.mi/hdswitch/server.sock
ffmpeg -y -i unix:///home/steven/projects/events.mi/hdswitch/server.sock -f webm - | ffmpeg2theora --output /dev/stdout --format webm - | oggfwd -p -n event icecast.mi.hdm-stuttgart.de 80 xV2kxUG3 /test.ogv

*/
// For accept4
#define _GNU_SOURCE

// Required for open_memstream
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include <poll.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "drawable.h"
#include "stb_image.h"
#include "cam.h"
#include "sound.h"
#include "timer.h"
#include "ebml_writer.h"
#include "array.h"


typedef struct {
	void*  data;
	size_t size;
	// Original start pointer, suitable for free()
	void*  ptr;
} buffer_t, *buffer_p;


int server_fd = -1;
size_t client_fd_cound = 0;
array_p client_fds = NULL;

char*  header_ptr = NULL;
size_t header_size = 0;
array_p buffers = NULL;


//FILE* webm_file = NULL;

void mkv_build_header(uint16_t width, uint16_t height) {
	//webm_file = fopen("video-scratchpad/test.webm", "wb");
	FILE* f = open_memstream(&header_ptr, &header_size);
	
	off_t o1, o2, o3, o4;
	
	o1 = ebml_element_start(f, MKV_EBML);
		ebml_element_string(f, MKV_DocType, "matroska");
	ebml_element_end(f, o1);
	
	ebml_element_start_unkown_data_size(f, MKV_Segment);
	
	o2 = ebml_element_start(f, MKV_Info);
		ebml_element_uint(f, MKV_TimecodeScale, 1000000);
		ebml_element_string(f, MKV_MuxingApp, "ebml_writer v0.1");
		ebml_element_string(f, MKV_WritingApp, "HDswitch v0.1");
	ebml_element_end(f, o2);
	
	o2 = ebml_element_start(f, MKV_Tracks);
		// Video track
		o3 = ebml_element_start(f, MKV_TrackEntry);
			ebml_element_uint(f, MKV_TrackNumber, 1);
			ebml_element_uint(f, MKV_TrackUID, 1);
			ebml_element_uint(f, MKV_TrackType, MKV_TrackType_Video);
			
			ebml_element_string(f, MKV_CodecID, "V_UNCOMPRESSED");
			// These were not included in files generated by mkclean
			//ebml_element_uint(f, MKV_FlagEnabled, 1);
			//ebml_element_uint(f, MKV_FlagDefault, 1);
			//ebml_element_uint(f, MKV_FlagForced, 1);
			ebml_element_uint(f, MKV_FlagLacing, 1);
			ebml_element_string(f, MKV_Language, "und");
			
			o4 = ebml_element_start(f, MKV_Video);
				ebml_element_uint(f, MKV_PixelWidth, width);
				ebml_element_uint(f, MKV_PixelHeight, height);
				ebml_element_string(f, MKV_ColourSpace, "YUY2");
			ebml_element_end(f, o4);
			
		ebml_element_end(f, o3);
		
		// Audio track
		o3 = ebml_element_start(f, MKV_TrackEntry);
			ebml_element_uint(f, MKV_TrackNumber, 2);
			ebml_element_uint(f, MKV_TrackUID, 2);
			ebml_element_uint(f, MKV_TrackType, MKV_TrackType_Audio);
			
			ebml_element_string(f, MKV_CodecID, "A_PCM/INT/LIT");
			ebml_element_uint(f, MKV_FlagLacing, 1);
			ebml_element_string(f, MKV_Language, "ger");
			
			o4 = ebml_element_start(f, MKV_Audio);
				ebml_element_float(f, MKV_SamplingFrequency, 48000);
				ebml_element_uint(f, MKV_Channels, 2);
				ebml_element_uint(f, MKV_BitDepth, 16);
			ebml_element_end(f, o4);
			
		ebml_element_end(f, o3);
		
	ebml_element_end(f, o2);
	
	fclose(f);
}
/*
void webm_write_frame(uint64_t timecode_ms, void* frame_data, size_t frame_size) {
	off_t o2, o3;
	FILE* f = webm_file;
	
	o2 = ebml_element_start(f, MKV_Cluster);
		ebml_element_uint(f, MKV_Timecode, timecode_ms);
		o3 = ebml_element_start(f, MKV_SimpleBlock);
			// Track number this frame belongs to
			ebml_write_data_size(f, 1, 0);
			
			int16_t block_timecode = 0;
			fwrite(&block_timecode, sizeof(block_timecode), 1, f);
			
			uint8_t flags = 0x80; // keyframe (1), reserved (000), not invisible (0), no lacing (00), not discardable (0)
			fwrite(&flags, sizeof(flags), 1, f);
			
			fwrite(frame_data, frame_size, 1, f);
		ebml_element_end(f, o3);
	ebml_element_end(f, o2);
}

void webm_write_audio_frame(uint64_t timecode_ms, void* frame_data, size_t frame_size) {
	off_t o2, o3;
	FILE* f = webm_file;
	
	o2 = ebml_element_start(f, MKV_Cluster);
		ebml_element_uint(f, MKV_Timecode, timecode_ms);
		o3 = ebml_element_start(f, MKV_SimpleBlock);
			// Track number this frame belongs to
			ebml_write_data_size(f, 2, 0);
			
			int16_t block_timecode = 0;
			fwrite(&block_timecode, sizeof(block_timecode), 1, f);
			
			uint8_t flags = 0x80; // keyframe (1), reserved (000), not invisible (0), no lacing (00), not discardable (0)
			fwrite(&flags, sizeof(flags), 1, f);
			
			fwrite(frame_data, frame_size, 1, f);
		ebml_element_end(f, o3);
	ebml_element_end(f, o2);
}

void webm_stop() {
	fclose(webm_file);
}
*/



bool server_start(uint16_t width, uint16_t height) {
	server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (server_fd == -1)
		return perror("socket"), false;
	
	struct sockaddr_un addr = { AF_UNIX, "server.sock" };
	if ( bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1 )
		return perror("bind"), false;
	
	if ( listen(server_fd, 3) == -1 )
		return perror("listen"), false;
	
	client_fds = array_of(int);
	mkv_build_header(width, height);
	
	buffers = array_of(buffer_t);
	
	return true;
}

void server_close() {
	for(size_t i = 0; i < client_fds->length; i++)
		close(array_elem(client_fds, int, i));
	array_destroy(client_fds);
	close(server_fd);
	
	array_destroy(buffers);
	free(header_ptr);
}

void server_accept() {
	int client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK);
	if (client_fd == -1) {
		perror("accept4");
		return;
	}
	
	array_append(client_fds, int, client_fd);
	void*  data = header_ptr;
	size_t size = header_size;
	ssize_t bytes_written = 0;
	while ( (bytes_written = write(client_fd, data, size)) > 0 ) {
		data += bytes_written;
		size -= bytes_written;
	}
	
	if (bytes_written < 0)
		perror("write");
	
	//write(client_fd, header_ptr, header_size);
	
}

void server_write_frame(uint8_t track, uint64_t timecode_ms, void* frame_data, size_t frame_size) {
	buffer_t buffer = { 0 };
	FILE* f = open_memstream((char**)&buffer.ptr, &buffer.size);
	
	off_t o2, o3;
	o2 = ebml_element_start(f, MKV_Cluster);
		ebml_element_uint(f, MKV_Timecode, timecode_ms);
		o3 = ebml_element_start(f, MKV_SimpleBlock);
			// Track number this frame belongs to
			ebml_write_data_size(f, track, 0);
			
			int16_t block_timecode = 0;
			fwrite(&block_timecode, sizeof(block_timecode), 1, f);
			
			uint8_t flags = 0x80; // keyframe (1), reserved (000), not invisible (0), no lacing (00), not discardable (0)
			fwrite(&flags, sizeof(flags), 1, f);
			
			fwrite(frame_data, frame_size, 1, f);
		ebml_element_end(f, o3);
	ebml_element_end(f, o2);
	
	fclose(f);
	buffer.data = buffer.ptr;
	
	if (client_fds->length > 0) {
		array_append(buffers, buffer_t, buffer);
	} else {
		// No clients connected, free buffer right away
		free(buffer.ptr);
	}
	
	for(size_t i = 0; i < client_fds->length; i++) {
		int client_fd = array_elem(client_fds, int, i);
		
		while (buffers->length > 0) {
			buffer_p current_buffer = buffers->data;
			
			ssize_t bytes_written = 0;
			while (current_buffer->size > 0) {
				bytes_written = write(client_fd, current_buffer->data, current_buffer->size);
				if (bytes_written < 0)
					break;
				
				current_buffer->data += bytes_written;
				current_buffer->size -= bytes_written;
			}
			
			if (bytes_written >= 0) {
				// Buffer finished, next one
				free(current_buffer->ptr);
				array_remove(buffers, 0);
			} else {
				// Didn't finished buffer, continue next time
				break;
			}
		}
	}
}


int main(int argc, char** argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: %s v4l2-file1 v4l2-file2 alsa-input\n", argv[0]);
		return 1;
	}
	
	//int v1w = 640, v1h = 480;  // video1 raw dimensions, best for HP Webcam HD 5210: 800x448
	int v1w = 800, v1h = 448;  // video1 raw dimensions, best for HP Webcam HD 5210: 
	int v2w = 640, v2h = 480;  // video2 raw dimensions
	int cw  = v1w, ch  = v1h;  // composite video dimensions
	int ww  = cw,  wh  = ch;   // window dimensions
	int cv1w = v1w,     cv1h = v1h,     cv1x = 0,         cv1y = 0;          // dimension and position of video1 in composite video
	int cv2w = v2w / 4, cv2h = v2h / 4, cv2x = cw - cv2w, cv2y = ch - cv2h;  // dimension and position of video2 in composite video
	
	
	
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
	atexit(SDL_Quit);
	
	SDL_Window* win = SDL_CreateWindow("HDSwitch", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
	//SDL_GL_SetSwapInterval(1);
	SDL_GL_SetSwapInterval(0);
	
	// Setup OpenGL stuff
	check_required_gl_extentions();
	
	GLuint video1_tex = texture_new(v1w, v1h, GL_RG8);
	GLuint video2_tex = texture_new(v2w, v2h, GL_RG8);
	GLuint composite_video_tex  = texture_new(cw, ch, GL_RGB8);
	GLuint stream_video_tex     = texture_new(cw, ch, GL_RG8);
	size_t stream_video_size = cw * ch * 2;
	void*  stream_video_ptr  = malloc(stream_video_size);
	
	GLuint main_tex = video1_tex, small_tex = video2_tex;
	
	
	// Initialize the textures so we don't get random GPU RAM garbage in
	// our first composite frame when one webcam frame hasn't been uploaded yet
	GLuint clear_fbo = 0;
	glGenFramebuffers(1, &clear_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, clear_fbo);
		// Black in YCbCr
		glClearColor(0, 0.5, 0, 0);
		
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, video1_tex, 0);
		if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return fprintf(stderr, "Framebuffer setup failed for clear 1\n"), 1;
		glViewport(0, 0, v1w, v1h);
		glClear(GL_COLOR_BUFFER_BIT);
	
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, video2_tex, 0);
		if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return fprintf(stderr, "Framebuffer setup failed for clear 2\n"), 1;
		glViewport(0, 0, v2w, v2h);
		glClear(GL_COLOR_BUFFER_BIT);
		
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &clear_fbo);
	
	fbo_p composite_video = fbo_new(composite_video_tex);
	
	// Build vertex buffers, use triangle strips for a basic quad. Quads were removed in OpenGL 3.2.
	drawable_p video_on_composite = drawable_new(GL_TRIANGLE_STRIP, "shaders/video_on_composite.vs", "shaders/video_on_composite.fs");
	GLuint video1_vertex_buffer = 0, video2_vertex_buffer = 0;
	{
		// B D
		// A C
		float tri_strip[] = {
			// pos x                         y                                    tex coord u, v
			 cv1x         / (cw / 2.0f) - 1, (cv1y + cv1h) / (ch / 2.0f) - 1,       0, v1h,
			 cv1x         / (cw / 2.0f) - 1,  cv1y         / (ch / 2.0f) - 1,       0,   0,
			(cv1x + cv1w) / (cw / 2.0f) - 1, (cv1y + cv1h) / (ch / 2.0f) - 1,     v1w, v1h,
			(cv1x + cv1w) / (cw / 2.0f) - 1,  cv1y         / (ch / 2.0f) - 1,     v1w, 0
		};
		video1_vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
	}
	{
		float tri_strip[] = {
			// pos x                         y                                    tex coord u, v
			 cv2x         / (cw / 2.0f) - 1, (cv2y + cv2h) / (ch / 2.0f) - 1,       0, v2h,
			 cv2x         / (cw / 2.0f) - 1,  cv2y         / (ch / 2.0f) - 1,       0,   0,
			(cv2x + cv2w) / (cw / 2.0f) - 1, (cv2y + cv2h) / (ch / 2.0f) - 1,     v2w, v2h,
			(cv2x + cv2w) / (cw / 2.0f) - 1,  cv2y         / (ch / 2.0f) - 1,     v2w, 0
		};
		video2_vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
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
	
	
	/*
	video->texture = texture_new(w, h, GL_RG8);
	
	// Triangle strip for a basic quad. Quads were removed in OpenGL 3.2.
	float tri_strip[] = {
		-1.0, -1.0,     0, h,
		-1.0,  1.0,     0, 0,
		 1.0, -1.0,     w, h,
		 1.0,  1.0,     w, 0
	};
	video->vertex_buffer = buffer_new(sizeof(tri_strip), tri_strip);
	
	drawable_p video2 = drawable_new(GL_TRIANGLE_STRIP, "shaders/video.vs", "shaders/video.fs");
	video2->texture = texture_new(w2, h2, GL_RG8);
	float tri_strip2[] = {
		0.25, -1.0,        0, h2,
		0.25, -0.4375,     0,  0,
		 1.0, -1.0,       w2, h2,
		 1.0, -0.4375,    w2,  0
	};
	video2->vertex_buffer = buffer_new(sizeof(tri_strip2), tri_strip2);
	*/
	
	// Setup sound input and output
	size_t latency = 5;
	
	sound_p in = sound_open(argv[3], SOUND_CAPTURE, SOUND_NONBLOCK);
	sound_configure(in, 48000, 2, SOUND_FORMAT_S16, true, latency);
	
	sound_p out = sound_open("default", SOUND_PLAYBACK, SOUND_NONBLOCK);
	sound_configure(out, 48000, 2, SOUND_FORMAT_S16, true, latency);
	
	size_t buffer_size = in->buffer_size;
	size_t buffer_filled = 0;
	void* buffer = malloc(in->buffer_size);
	
	// Setup camera
	cam_p cam = cam_open(argv[1]);
	cam_print_info(cam);
	cam_setup(cam, __builtin_bswap32('YUYV'), v1w, v1h, 30, 1, NULL);
	cam_print_frame_rate(cam);
	
	cam_p cam2 = cam_open(argv[2]);
	cam_print_info(cam2);
	cam_setup(cam2, __builtin_bswap32('YUYV'), v2w, v2h, 30, 1, NULL);
	cam_print_frame_rate(cam2);
	
	
	server_start(cw, ch);
	
	cam_stream_start(cam, 2);
	cam_stream_start(cam2, 2);
	
	
	// Prepare mainloop
	size_t poll_fd_count = 3 + sound_poll_fds_count(in) + sound_poll_fds_count(out);
	struct pollfd pollfds[poll_fd_count];
	
	timeval_t mark;
	uint32_t start_ms = SDL_GetTicks();
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
				
				if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
					ww = event.window.data1;
					wh = event.window.data2;
				}
				
				if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_s) {
					main_tex ^= small_tex;
					small_tex ^= main_tex;
					main_tex ^= small_tex;
				}
			}
		double sdl_time = time_mark(&mark);
		
		// Setup and do the poll
		time_mark(&mark);
			size_t poll_fds_used = 0;
			
			pollfds[0] = (struct pollfd){ .fd = cam->fd, .events = POLLIN, .revents = 0 };
			poll_fds_used++;
			pollfds[1] = (struct pollfd){ .fd = cam2->fd, .events = POLLIN, .revents = 0 };
			poll_fds_used++;
			pollfds[2] = (struct pollfd){ .fd = server_fd, .events = POLLIN, .revents = 0 };
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
			server_write_frame(2, SDL_GetTicks() - start_ms, buffer + buffer_filled, bytes_read);
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
		
		bool something_to_render = false;
		
		// Render new video frames
		if (pollfds[0].revents & POLLIN) {
			mark = time_now();
			
			cam_buffer_t frame = cam_frame_get(cam);
			cam_time += time_mark(&mark);
			
			texture_update(video1_tex, GL_RG, frame.ptr);
			tex_update_time += time_mark(&mark);
			
			cam_frame_release(cam);
			
			something_to_render = true;
		}
		
		if (pollfds[1].revents & POLLIN) {
			mark = time_now();
			
			cam_buffer_t frame = cam_frame_get(cam2);
			cam_time += time_mark(&mark);
			
			texture_update(video2_tex, GL_RG, frame.ptr);
			tex_update_time += time_mark(&mark);
			
			cam_frame_release(cam2);
			
			something_to_render = true;
		}
		
		if (something_to_render) {
			mark = time_now();
			
			fbo_bind(composite_video);
				// Clear rest to black (luma 0, cb 0, cr 0 but croma channels are not in [-0.5, 0.5] but in [0, 1])
				glClearColor(0, 0.5, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT);
				
				video_on_composite->texture = main_tex;
				video_on_composite->vertex_buffer = video1_vertex_buffer;
				drawable_draw(video_on_composite);
				
				video_on_composite->texture = small_tex;
				video_on_composite->vertex_buffer = video2_vertex_buffer;
				drawable_draw(video_on_composite);
				
			fbo_bind(stream_fbo);
				
				drawable_draw(stream);
				
			fbo_bind(NULL);
			
			fbo_read(stream_fbo, GL_RG, GL_UNSIGNED_BYTE, stream_video_ptr);
			server_write_frame(1, SDL_GetTicks() - start_ms, stream_video_ptr, stream_video_size);
			
			glViewport(0, 0, ww, wh);
			
			drawable_draw(gui);
			draw_time = time_mark(&mark);
			
			SDL_GL_SwapWindow(win);
			swap_time = time_mark(&mark);
		}
		
		// Accept new clients
		if (pollfds[2].revents & POLLIN)
			server_accept();
		
		
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
	cam_stream_stop(cam2);
	cam_close(cam2);
	
	server_close();
	
	drawable_destroy(stream);
	drawable_destroy(gui);
	drawable_destroy(video_on_composite);
	
	fbo_destroy(stream_fbo);
	fbo_destroy(composite_video);
	texture_destroy(video1_tex);
	texture_destroy(video2_tex);
	texture_destroy(composite_video_tex);
	free(stream_video_ptr);
	
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	return 0;
}