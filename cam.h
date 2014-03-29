#pragma once

#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>


typedef struct {
	size_t size;
	void*  ptr;
} cam_buffer_t, *cam_buffer_p;

typedef struct {
	int fd;
	size_t buffer_count;
	cam_buffer_p buffers;
	ssize_t dequeued_buffer;
} cam_t, *cam_p;

typedef struct {
	const char* name;
	const char* value;
} cam_control_t, *cam_control_p;


cam_p cam_open(const char* camera_file);
void  cam_close(cam_p cam);

void  cam_setup(cam_p cam, uint32_t pixel_format, uint32_t width, uint32_t height, uint32_t frame_rate_num, uint32_t frame_rate_den, cam_control_t controls[]);
bool  cam_set_controls(cam_p cam, cam_control_t controls[]);
void  cam_print_info(cam_p cam);
bool  cam_print_frame_rate(cam_p cam);

bool  cam_stream_start(cam_p cam, size_t buffer_count);
bool  cam_stream_stop(cam_p cam);

cam_buffer_t cam_frame_get(cam_p cam);
bool         cam_frame_release(cam_p cam);


/**
 * Allows to use four character character literals to specify the pixel format. Example:
 * 
 *   cam_pixel_format('YUYV')
 * 
 * This works because character literals return integers. But we need to convert from little
 * endian (x86) to big endian (as you see it in a hex editor).
 */
static inline uint32_t cam_pixel_format(uint32_t pixel_format_as_char_literal){
	return __builtin_bswap32(pixel_format_as_char_literal);
}