#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <linux/videodev2.h>
#include "cam.h"


static void show_device_capabilities(int fd);
static void show_capture_pixel_formats(int fd);
static void show_resolutions(int fd, uint32_t pixel_format);
static void show_resolution_frame_rates(int fd, uint32_t pixel_format, uint32_t width, uint32_t height);
static void show_controls(int fd);
static bool set_pixel_format_and_resolution(cam_p cam, uint32_t pixel_format, uint32_t width, uint32_t height);
static bool set_frame_rate(cam_p cam, uint32_t frame_interval_num, uint32_t frame_interval_den);
static bool set_controls(cam_p cam, cam_control_t controls[]);
static bool map_buffers(cam_p cam, size_t buffer_count);
static bool unmap_buffers(cam_p cam);


/**
 * Opens the v4l2 device.
 * 
 * It is opened in read-write mode since we need a read-write mapping for mmap() later on. Otherwise the
 * USB webcam driver doesn't work (the PS3Eye will work though).
 */
cam_p cam_open(const char* camera_file){
	cam_p cam = malloc(sizeof(cam_t));
	
	cam->fd = open(camera_file, O_RDWR);
	if (cam->fd == -1){
		perror("camera: open() of video device failed");
		free(cam);
		return NULL;
	}
	
	cam->buffer_count = 0;
	cam->buffers = NULL;
	cam->dequeued_buffer = -1;
	
	return cam;
}

/**
 * Closes the camera. Stops streaming if necessary before closing.
 */
void cam_close(cam_p cam){
	if (cam->buffers != NULL)
		cam_stream_stop(cam);
	
	close(cam->fd);
	free(cam);
}


void cam_setup(cam_p cam, uint32_t pixel_format, uint32_t width, uint32_t height, uint32_t frame_rate_num, uint32_t frame_rate_den, cam_control_t controls[]){
	set_pixel_format_and_resolution(cam, pixel_format, width, height);
	set_frame_rate(cam, frame_rate_num, frame_rate_den);
	set_controls(cam, controls);
}

void cam_print_info(cam_p cam){
	show_device_capabilities(cam->fd);
	show_capture_pixel_formats(cam->fd);
	show_controls(cam->fd);
}


bool cam_stream_start(cam_p cam, size_t buffer_count){
	if ( ! map_buffers(cam, buffer_count) )
		return false;
	
	// Enqueue all buffers and turn on streaming
	struct v4l2_buffer buffer = {0};
	buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory = V4L2_MEMORY_MMAP;
	
	for(size_t i = 0; i < cam->buffer_count; i++){
		buffer.index = i;
		if ( ioctl(cam->fd, VIDIOC_QBUF, &buffer) == -1 ){
			perror("VIDIOC_QBUF ioctl() failed");
			break;
		}
	}
	
	int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if ( ioctl(cam->fd, VIDIOC_STREAMON, &stream_type) == -1 ) {
		perror("VIDIOC_STREAMON ioctl() failed");
		return false;
	}
	
	return true;
}

bool cam_stream_stop(cam_p cam){
	// Stop streaming, this also dequeues all buffers
	int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if ( ioctl(cam->fd, VIDIOC_STREAMOFF, &stream_type) == -1 ) {
		perror("VIDIOC_STREAMOFF ioctl() failed");
		return false;
	}
	
	unmap_buffers(cam);
	
	return true;
}


cam_buffer_t cam_frame_get(cam_p cam){
	if ( cam->dequeued_buffer != -1 ) {
		fprintf(stderr, "Can't get a video buffer while another buffer is still not released\n");
		return (cam_buffer_t){ .size = 0, .ptr = NULL };
	}
	
	struct v4l2_buffer buffer = {0};
	buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory = V4L2_MEMORY_MMAP;
	
	if ( ioctl(cam->fd, VIDIOC_DQBUF, &buffer) == -1 ) {
		perror("VIDIOC_DQBUF ioctl() failed");
		return (cam_buffer_t){ .size = 0, .ptr = NULL };
	}
	
	cam->dequeued_buffer = buffer.index;
	return (cam_buffer_t){ .size = buffer.bytesused, .ptr = cam->buffers[buffer.index].ptr };
}

bool cam_frame_release(cam_p cam){
	if ( cam->dequeued_buffer == -1 ) {
		fprintf(stderr, "Can't release a video buffer without getting one first\n");
		return false;
	}
	
	// Enque the buffer in the video capture queue so the driver can use it again
	struct v4l2_buffer buffer = {0};
	buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = cam->dequeued_buffer;
	
	if ( ioctl(cam->fd, VIDIOC_QBUF, &buffer) == -1 ) {
		perror("VIDIOC_QBUF ioctl() failed");
		return false;
	}
	
	cam->dequeued_buffer = -1;
	return true;
}


static void show_device_capabilities(int fd){
	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(struct v4l2_capability));
	
	printf("Device capabilities:\n");
	if ( ioctl(fd, VIDIOC_QUERYCAP, &cap) != -1 ) {
		printf("  driver: %s\n", cap.driver);
		printf("  card: %s\n", cap.card);
		printf("  bus_info: %s\n", cap.bus_info);
		
		#define PRINT_CAP(flag, name) if((cap.capabilities & flag) == flag) printf("  - %s\n", name);
		
		printf("  capabilities:\n");
		PRINT_CAP(V4L2_CAP_VIDEO_CAPTURE,        "V4L2_CAP_VIDEO_CAPTURE");
		PRINT_CAP(V4L2_CAP_VIDEO_OUTPUT,         "V4L2_CAP_VIDEO_OUTPUT");
		PRINT_CAP(V4L2_CAP_VIDEO_OVERLAY,        "V4L2_CAP_VIDEO_OVERLAY");
		PRINT_CAP(V4L2_CAP_VBI_CAPTURE,          "V4L2_CAP_VBI_CAPTURE");
		PRINT_CAP(V4L2_CAP_VBI_OUTPUT,           "V4L2_CAP_VBI_OUTPUT");
		PRINT_CAP(V4L2_CAP_SLICED_VBI_CAPTURE,   "V4L2_CAP_SLICED_VBI_CAPTURE");
		PRINT_CAP(V4L2_CAP_SLICED_VBI_OUTPUT,    "V4L2_CAP_SLICED_VBI_OUTPUT");
		PRINT_CAP(V4L2_CAP_RDS_CAPTURE,          "V4L2_CAP_RDS_CAPTURE");
		PRINT_CAP(V4L2_CAP_VIDEO_OUTPUT_OVERLAY, "V4L2_CAP_VIDEO_OUTPUT_OVERLAY");
		PRINT_CAP(V4L2_CAP_HW_FREQ_SEEK,         "V4L2_CAP_HW_FREQ_SEEK");
		PRINT_CAP(V4L2_CAP_RDS_OUTPUT,           "V4L2_CAP_RDS_OUTPUT");
		PRINT_CAP(V4L2_CAP_TUNER,                "V4L2_CAP_TUNER");
		PRINT_CAP(V4L2_CAP_AUDIO,                "V4L2_CAP_AUDIO");
		PRINT_CAP(V4L2_CAP_RADIO,                "V4L2_CAP_RADIO");
		PRINT_CAP(V4L2_CAP_MODULATOR,            "V4L2_CAP_MODULATOR");
		PRINT_CAP(V4L2_CAP_READWRITE,            "V4L2_CAP_READWRITE");
		PRINT_CAP(V4L2_CAP_ASYNCIO,              "V4L2_CAP_ASYNCIO");
		PRINT_CAP(V4L2_CAP_STREAMING,            "V4L2_CAP_STREAMING");
		
		#undef PRINT_CAP
	} else {
		perror("  ioctl() VIDIOC_QUERYCAP");
	}
}

static void show_capture_pixel_formats(int fd){
	struct v4l2_fmtdesc format = {0};
	format.index = 0;
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	
	printf("Supported V4L2_BUF_TYPE_VIDEO_CAPTURE pixel formats:\n");
	while( ioctl(fd, VIDIOC_ENUM_FMT, &format) != -1 ) {
		printf("- %.4s: %s, flags: 0x%04x\n",
			(const char*)&format.pixelformat, format.description, format.flags);
		show_resolutions(fd, format.pixelformat);
		
		format.index++;
	}
	
	// The format list will end as soon as ioctl() returns EINVAL for the
	// specified format index. If another error was returned something went wrong
	// so show it to the user.
	if (errno != EINVAL)
		perror("  ioctl() VIDIOC_ENUM_FMT");
}

static void show_resolutions(int fd, uint32_t pixel_format){
	struct v4l2_frmsizeenum framesize = {0};
	framesize.index = 0;
	framesize.pixel_format = pixel_format;
	
	while( ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &framesize) != -1 ) {
		if (framesize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
			printf("  %ux%u:", framesize.discrete.width, framesize.discrete.height);
			show_resolution_frame_rates(fd, pixel_format, framesize.discrete.width, framesize.discrete.height);
		} else {
			fprintf(stderr, "stepwise or continuous resolutions not supported\n");
			// For these types we only have one frame size entry with index 0
			// so end the loop now.
			break;
		}
		
		framesize.index++;
	}
	
	// The format list will end as soon as ioctl() returns EINVAL for the
	// specified format index. If another error was returned something went wrong
	// so show it to the user.
	if (errno != EINVAL)
		perror("  ioctl() VIDIOC_ENUM_FRAMESIZES");
}

static void show_resolution_frame_rates(int fd, uint32_t pixel_format, uint32_t width, uint32_t height){
	struct v4l2_frmivalenum frame_interval = {0};
	frame_interval.index = 0;
	frame_interval.pixel_format = pixel_format;
	frame_interval.width = width;
	frame_interval.height = height;
	
	while ( ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frame_interval) != -1 ) {
		if (frame_interval.type != V4L2_FRMIVAL_TYPE_DISCRETE) {
			printf(" only descrete frame intervals are supported\n");
			return;
		}
		
		if (frame_interval.discrete.numerator == 1)
			printf(" %u", frame_interval.discrete.denominator);
		else
			printf(" %u/%u", frame_interval.discrete.numerator, frame_interval.discrete.denominator);
		frame_interval.index++;
	}
	printf(" fps\n");
	
	if (errno != EINVAL)
		perror("  ioctl() VIDIOC_ENUM_FRAMEINTERVALS");
}

static void show_controls(int fd){
	// Array to translate type constants to display names
	const char* type_names[] = {
		NULL,
		"integer",
		"boolean",
		"menu",
		"button",
		"integer64",
		"ctrl_class",
		"string",
		"bitmask",
		"integer_menu"
	};
	struct v4l2_queryctrl control = {0};
	
	printf("Controls:\n");
	for(control.id = V4L2_CID_BASE; control.id < V4L2_CID_LASTP1; control.id++){
		if ( ioctl(fd, VIDIOC_QUERYCTRL, &control) == -1 ) {
			if (errno == EINVAL)
				continue;
			perror("VIDIOC_QUERYCTRL ioctl() failed");
			break;
		}
		
		if (control.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;
		
		struct v4l2_control control_value = {control.id, 0};
		if ( ioctl(fd, VIDIOC_G_CTRL, &control_value) == -1 )
			perror("VIDIOC_G_CTRL ioctl() failed");
		
		if (control.type == V4L2_CTRL_TYPE_MENU || control.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
			printf("- %s: %d (%s)\n", control.name, control_value.value, type_names[control.type]);
			
			struct v4l2_querymenu menu_entry = {0};
			for(ssize_t i = control.minimum; i <= control.maximum; i++){
				menu_entry.id = control.id;
				menu_entry.index = i;
				
				if ( ioctl(fd, VIDIOC_QUERYMENU, &menu_entry) == -1 ) {
					perror("VIDIOC_QUERYMENU ioctl() failed");
					continue;
				}
				
				if (control.type == V4L2_CTRL_TYPE_MENU)
					printf("  %d: %s\n", menu_entry.index, menu_entry.name);
				else
					printf("  %d: %lld\n", menu_entry.index, menu_entry.value);
			}
			
		} else {
			printf("- %s: %d (%s, values in [%d, %d] step %d)\n",
				control.name, control_value.value, type_names[control.type], control.minimum, control.maximum, control.step);
		}
	}
}


static bool set_pixel_format_and_resolution(cam_p cam, uint32_t pixel_format, uint32_t width, uint32_t height){
	struct v4l2_format format;
	memset(&format, 0, sizeof(format));
	
	// Get the current pixel format (best practice mentioned in the V4L2 spec)
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if ( ioctl(cam->fd, VIDIOC_G_FMT, &format) == -1 ) {
		perror("VIDIOC_G_FMT ioctl() failed");
		return false;
	}
	
	// Set the requested resolution and set the pixel format
	format.fmt.pix.pixelformat = pixel_format;
	format.fmt.pix.width = width;
	format.fmt.pix.height = height;
	if ( ioctl(cam->fd, VIDIOC_S_FMT, &format) == -1 ) {
		perror("VIDIOC_S_FMT ioctl() failed");
		return false;
	}
	
	return true;
}

static bool set_frame_rate(cam_p cam, uint32_t frame_rate_num, uint32_t frame_rate_den){
	struct v4l2_streamparm params = {0};
	
	params.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if ( ioctl(cam->fd, VIDIOC_G_PARM, &params) == -1 ) {
		perror("VIDIOC_G_PARM ioctl() failed");
		return false;
	}
	
	if ( !(params.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) ) {
		fprintf(stderr, "frame rate setting not supported\n");
		return false;
	}
	
	// We get the frame rate (number of frames per second) but v4l2 uses
	// the frame interval (time between two frames). Therefore use the
	// reciprocal of the frame rate.
	params.parm.capture.timeperframe.numerator = frame_rate_den;
	params.parm.capture.timeperframe.denominator = frame_rate_num;
	
	if ( ioctl(cam->fd, VIDIOC_S_PARM, &params) == -1 ) {
		perror("VIDIOC_S_PARM ioctl() failed");
		return false;
	}
	
	return true;
}

/**
 * Sets the v4l2 user controls to values specified by the user. Example:
 * 
 *   set_controls(cam, (cam_control_t[]){
 *     { "Contrast", "0" },
 *     { "Power Line Frequency", "50 Hz" },
 *     { "Sharpness", "2" },
 *     { "Gamma", "110" },
 *     { NULL, NULL }
 *   });
 * 
 * The list is terminated by an entry with the name `NULL`. The control names used have
 * to match the control names provided by the camera driver. For menu controls the name
 * of the menu item has to be specified as value. For other control types the value is
 * set as an integer.
 */
static bool set_controls(cam_p cam, cam_control_t controls[]){
	// Do nothing if we get no controls array
	if (controls == NULL)
		return true;
	
	struct v4l2_queryctrl control = {0};
	
	for(control.id = V4L2_CID_BASE; control.id < V4L2_CID_LASTP1; control.id++){
		if ( ioctl(cam->fd, VIDIOC_QUERYCTRL, &control) == -1 ) {
			if (errno == EINVAL)
				continue;
			perror("VIDIOC_QUERYCTRL ioctl() failed");
			break;
		}
		
		if (control.flags & V4L2_CTRL_FLAG_DISABLED)
			continue;
		
		// Look through the controls array and look if we have a value to set
		cam_control_p value_to_set = NULL;
		for(cam_control_p c = controls; c->name != NULL; c++){
			if ( strcmp((char*)control.name, c->name) == 0 ) {
				value_to_set = c;
				break;
			}
		}
		
		// User does not want to set a value for this control, try next control
		if (value_to_set == NULL)
			continue;
		
		struct v4l2_control control_value;
		control_value.id = control.id;
		
		if (control.type == V4L2_CTRL_TYPE_MENU) {
			// Find the index of the menu entry the user wants
			ssize_t entry_index = -1;
			
			struct v4l2_querymenu menu_entry = {0};
			for(ssize_t i = control.minimum; i <= control.maximum; i++){
				menu_entry.id = control.id;
				menu_entry.index = i;
				
				if ( ioctl(cam->fd, VIDIOC_QUERYMENU, &menu_entry) == -1 ) {
					perror("VIDIOC_QUERYMENU ioctl() failed");
					continue;
				}
				
				if ( strcmp((char*)menu_entry.name, value_to_set->value) == 0 ) {
					entry_index = menu_entry.index;
					break;
				}
			}
			
			if (entry_index != -1) {
				control_value.value = entry_index;
			} else {
				// Index for menu entry not found, try next control
				break;
			}
		} else {
			// Just set the integer value specified by the user (even for V4L2_CTRL_TYPE_INTEGER_MENU controls)
			control_value.value = strtol(value_to_set->value, NULL, 10);
		}
		
		if ( ioctl(cam->fd, VIDIOC_S_CTRL, &control_value) == -1 )
			perror("VIDIOC_G_CTRL ioctl() failed");
	}
	
	return true;
}

static bool map_buffers(cam_p cam, size_t buffer_count){
	// Request the video buffers
	struct v4l2_requestbuffers reqbuf = {0};
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = V4L2_MEMORY_MMAP;
	reqbuf.count = buffer_count;
	
	if ( ioctl(cam->fd, VIDIOC_REQBUFS, &reqbuf) == -1 ) {
		if (errno == EINVAL)
			fprintf(stderr, "Video capturing or mmap-streaming is not supported\n");
		else
			perror("VIDIOC_REQBUFS ioctl() failed");
		return false;
	}
	
	// Create memory maps for all buffers, reqbuf.count now contains the actual number of
	// buffers allocated by the driver.
	cam->buffer_count = reqbuf.count;
	cam->buffers = calloc(cam->buffer_count, sizeof(cam_buffer_t));
	
	for (size_t i = 0; i < cam->buffer_count; i++){
		struct v4l2_buffer buffer = {0};
		buffer.type = reqbuf.type;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;
		
		if ( ioctl(cam->fd, VIDIOC_QUERYBUF, &buffer) == -1 ) {
			perror("VIDIOC_QUERYBUF ioctl() failed");
			break;
		}
		
		cam->buffers[i].size = buffer.length;
		cam->buffers[i].ptr = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, buffer.m.offset);
		if (cam->buffers[i].ptr == MAP_FAILED) {
			cam->buffers[i].ptr = NULL;
			free(cam->buffers);
			perror("mmap() of video buffer failed");
			return false;
		}
	}
	
	return true;
}

static bool unmap_buffers(cam_p cam){
	// Free the video buffer memory maps
	for (size_t i = 0; i < cam->buffer_count; i++)
		munmap(cam->buffers[i].ptr, cam->buffers[i].size);
	
	// Free the buffer list
	free(cam->buffers);
	
	cam->buffer_count = 0;
	cam->buffers = NULL;
	
	return true;
}