#include <stdio.h>
#include "../cam.h"

int main(int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s v4l2-file\n", argv[0]);
		return 1;
	}
	
	cam_p cam = cam_open(argv[1]);
	cam_print_info(cam);
	cam_setup(cam, __builtin_bswap32('YUYV'), 640, 480, 1, 30, NULL);
	
	cam_stream_start(cam, 2);
	for(size_t i = 0; i < 100; i++) {
		cam_buffer_t frame = cam_frame_get(cam);
		printf("%zu bytes: %p\n", frame.size, frame.ptr);
		cam_frame_release(cam);
	}
	cam_stream_stop(cam);
	
	cam_close(cam);
	
	return 0;
}