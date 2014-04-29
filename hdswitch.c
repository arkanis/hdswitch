/**

Test commands for output:

ffplay unix:server.sock
ffmpeg -y -i unix:server.sock -f webm - | ffmpeg2theora --output /dev/stdout --format webm - | oggfwd -p -n event icecast.mi.hdm-stuttgart.de 80 xV2kxUG3 /test.ogv

*/

// For accept4() and open_memstream() (needs _POSIX_C_SOURCE 200809L which is also defined by _GNU_SOURCE)
// sigemptyset() and co. (need _POSIX_SOURCE)
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <SDL/SDL.h>
#include <pulse/pulseaudio.h>
#include <poll.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include "drawable.h"
#include "stb_image.h"
#include "cam.h"
#include "timer.h"
#include "ebml_writer.h"
#include "array.h"
#include "list.h"


// Config variables
const uint32_t latency_ms = 10;
const uint32_t mixer_buffer_time_ms = 1000;
const uint32_t max_latency_for_mixer_block_ms = 20;


static int signals_init();
static int signals_cleanup(int signal_fd);

static void signals_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
static void sdl_event_check_cb(pa_mainloop_api *a, pa_time_event *e, const struct timeval *tv, void *userdata);
static void camera_frame_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
void server_queue_frame(pa_mainloop_api *mainloop, uint8_t track, uint64_t timecode_ms, void* frame_data, size_t frame_size);

// Global stuff used by event callbacks
size_t scene_count = 0;
size_t scene_idx = 0;
size_t ww, wh;
bool something_to_render = false;
uint32_t start_ms = 0;

usec_t global_start_walltime = 0;


//
// Mixer stuff
//

typedef struct mic_s mic_t, *mic_p;
struct mic_s {
	ssize_t bytes_in_mixer_buffer;
	pa_stream* stream;
	char* name;
	usec_t start;
	uint64_t pts;
	mic_p next;
};
mic_p mics = NULL;
pa_stream* audio_playback_stream = NULL;


pa_sample_spec mixer_sample_spec = {
	.format   = PA_SAMPLE_S16LE,
	.rate     = 48000,
	.channels = 2
};

void*    mixer_buffer_ptr = NULL;
size_t   mixer_buffer_size = 0;
uint64_t mixer_pts = 0;

pa_context* context = NULL;
pa_mainloop_api* global_mainloop = NULL;
uint16_t log_countdown = 0;

static void mixer_on_context_state_changed(pa_context *c, void *userdata);
static void source_info_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static void mixer_on_new_mic_data(pa_stream *s, size_t length, void *userdata);

void mixer_init(pa_mainloop_api* mainloop) {
	mixer_buffer_size = pa_usec_to_bytes(mixer_buffer_time_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
	mixer_buffer_ptr = malloc(mixer_buffer_size);
	memset(mixer_buffer_ptr, 0, mixer_buffer_size);
	
	context = pa_context_new(mainloop, "HDswitch");
	pa_context_set_state_callback(context, mixer_on_context_state_changed, NULL);
	pa_context_connect(context, NULL, 0, NULL);
}

void mixer_cleanup() {
	free(mixer_buffer_ptr);
	pa_context_disconnect(context);
}

static void mixer_on_context_state_changed(pa_context *c, void *userdata) {
	switch (pa_context_get_state(c)) {
		case PA_CONTEXT_UNCONNECTED:  printf("PA_CONTEXT_UNCONNECTED\n");  break;
		case PA_CONTEXT_CONNECTING:   printf("PA_CONTEXT_CONNECTING\n");   break;
		case PA_CONTEXT_AUTHORIZING:  printf("PA_CONTEXT_AUTHORIZING\n");  break;
		case PA_CONTEXT_SETTING_NAME: printf("PA_CONTEXT_SETTING_NAME\n"); break;
		case PA_CONTEXT_READY:        printf("PA_CONTEXT_READY\n");
			{
				pa_operation* op = pa_context_get_source_info_list(c, source_info_list_cb, NULL);
				pa_operation_unref(op);
			}
			break;
		case PA_CONTEXT_FAILED:       printf("PA_CONTEXT_FAILED\n");     break;
		case PA_CONTEXT_TERMINATED:   printf("PA_CONTEXT_TERMINATED\n"); break;
		//default: break;
	}
}


static void source_info_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
	// Source list finished, create the playback stream
	if (i == NULL) {
		return;
	}
	
	// Ignore all monitor streams, we only want real mics
	if ( !(i->flags & PA_SOURCE_HARDWARE) )
		return;
	
	printf("name: %s\n", i->name);
	printf("  index: %u\n", i->index);
	printf("  description: %s\n", i->description);
	printf("  driver: %s\n", i->driver);
	printf("  latency: %.1f ms, configured: %.1f ms\n", i->latency / 1000.0, i->configured_latency / 1000.0);
	
	char buffer[512] = { 0 };
	pa_sample_spec_snprint(buffer, sizeof(buffer), &i->sample_spec);
	printf("  sample_spec: %s\n", buffer);
	
	pa_channel_map_snprint(buffer, sizeof(buffer), &i->channel_map);
	printf("  channel_map: %s\n", buffer);
	
	printf("  flags:\n");
	if (i->flags & PA_SOURCE_HW_VOLUME_CTRL)  printf("  - PA_SOURCE_HW_VOLUME_CTRL\n");
	if (i->flags & PA_SOURCE_LATENCY)         printf("  - PA_SOURCE_LATENCY\n");
	if (i->flags & PA_SOURCE_HARDWARE)        printf("  - PA_SOURCE_HARDWARE\n");
	if (i->flags & PA_SOURCE_NETWORK)         printf("  - PA_SOURCE_NETWORK\n");
	if (i->flags & PA_SOURCE_HW_MUTE_CTRL)    printf("  - PA_SOURCE_HW_MUTE_CTRL\n");
	if (i->flags & PA_SOURCE_DECIBEL_VOLUME)  printf("  - PA_SOURCE_DECIBEL_VOLUME\n");
	if (i->flags & PA_SOURCE_DYNAMIC_LATENCY) printf("  - PA_SOURCE_DYNAMIC_LATENCY\n");
	if (i->flags & PA_SOURCE_FLAT_VOLUME)     printf("  - PA_SOURCE_FLAT_VOLUME\n");
	
	printf("  properties:\n");
	const char* key = NULL;
	void* it = NULL;
	while ( (key = pa_proplist_iterate(i->proplist, &it)) != NULL ) {
		printf("  - %s: %s\n", key, pa_proplist_gets(i->proplist, key));
	}
	
	mic_p mic = malloc(sizeof(mic_t));
	mic->bytes_in_mixer_buffer = -1;
	mic->next = mics;
	mic->stream = pa_stream_new(c, "HDswitch", &mixer_sample_spec, NULL);
	mic->name = strdup(i->description);
	mics = mic;
	
	pa_stream_set_read_callback(mic->stream, mixer_on_new_mic_data, mic);
	
	pa_buffer_attr buffer_attr = { 0 };
	buffer_attr.maxlength = (uint32_t) -1;
	buffer_attr.prebuf = (uint32_t) -1;
	buffer_attr.fragsize = pa_usec_to_bytes(latency_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
	buffer_attr.tlength = (uint32_t) -1;
	buffer_attr.minreq = (uint32_t) -1;
	
	if ( pa_stream_connect_record(mic->stream, i->name, &buffer_attr, PA_STREAM_ADJUST_LATENCY | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE) < 0 ) {
		printf("pa_stream_connect_record() failed: %s", pa_strerror(pa_context_errno(c)));
	}
}


static void mixer_on_new_mic_data(pa_stream *s, size_t length, void *userdata) {
	const void *in_buffer_ptr;
	size_t in_buffer_size;
	mic_p stream_data = userdata;
	/*
	if (stream_data->bytes_in_mixer_buffer == -1) {
		stream_data->bytes_in_mixer_buffer = 0;
		
		if (audio_playback_stream == NULL) {
			audio_playback_stream = pa_stream_new(context, "HDswitch", &mixer_sample_spec, NULL);
			
			pa_buffer_attr buffer_attr = { 0 };
			buffer_attr.maxlength = (uint32_t) -1;
			buffer_attr.prebuf = (uint32_t) -1;
			buffer_attr.fragsize = (uint32_t) -1;
			buffer_attr.tlength = pa_usec_to_bytes(latency_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
			buffer_attr.minreq = (uint32_t) -1;
			
			pa_stream_connect_playback(audio_playback_stream, NULL, &buffer_attr, PA_STREAM_ADJUST_LATENCY, NULL, NULL);
			
			char buffer[512] = { 0 };
			pa_sample_spec_snprint(buffer, sizeof(buffer), pa_stream_get_sample_spec(audio_playback_stream));
			printf("audio_playback_stream sample_spec: %s\n", buffer);
		}
	}
	*/
	
	
	bool init_stream_if_latency_known_or_drop_data() {
		pa_usec_t latency = 0;
		int negative = 0;
		int result = pa_stream_get_latency(s, &latency, &negative);
		
		if (result != -PA_ERR_NODATA) {
			printf("  stream latency: %.2lf ms\n", latency / 1000.0);
			
			stream_data->bytes_in_mixer_buffer = 0;
			stream_data->pts = time_now() - latency - global_start_walltime;
			
			printf("  pts: %.2lf ms\n", stream_data->pts / 1000.0);
			
			if (mixer_pts == 0)
				mixer_pts = stream_data->pts;
			
			// If the stream latency is above the mixer block threshold don't make
			// the mixer wait (block) for this stream but instead mix it as we get it.
			// This way the user might be able to detect the faulty mic with to much latency.
			if (latency / 1000 > max_latency_for_mixer_block_ms) {
				stream_data->pts = mixer_pts;
				printf("  IGNORING MIC LATENCY because it's to high! We don't want everything to wait for it.\n");
			}
			
			return true;
		} else {
			printf("  no stream latency data yet!\n");
			
			while (pa_stream_readable_size(s) > 0) {
				if (pa_stream_peek(s, &in_buffer_ptr, &in_buffer_size) < 0) {
					fprintf(stderr, "  Read failed\n");
					return false;
				}
				pa_stream_drop(s);
			}
			
			return false;
		}
	}
	
	
	// Initialize the byte offset only as soon as we got the first data packet.
	// Otherwise always waits for this stream from it's start to the arival of the first packet.
	if (stream_data->bytes_in_mixer_buffer == -1) {
		stream_data->bytes_in_mixer_buffer = -2;
		
		if (audio_playback_stream == NULL) {
			audio_playback_stream = pa_stream_new(context, "HDswitch", &mixer_sample_spec, NULL);
			
			pa_buffer_attr buffer_attr = { 0 };
			buffer_attr.maxlength = (uint32_t) -1;
			buffer_attr.prebuf = (uint32_t) -1;
			buffer_attr.fragsize = (uint32_t) -1;
			buffer_attr.tlength = pa_usec_to_bytes(latency_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
			buffer_attr.minreq = (uint32_t) -1;
			
			pa_stream_connect_playback(audio_playback_stream, NULL, &buffer_attr, PA_STREAM_ADJUST_LATENCY, NULL, NULL);
			
			char buffer[512] = { 0 };
			pa_sample_spec_snprint(buffer, sizeof(buffer), pa_stream_get_sample_spec(audio_playback_stream));
			printf("audio_playback_stream sample_spec: %s\n", buffer);
		}
		
		printf("[%p] start after %.2lf ms, initial data: %zu bytes, %.2lf ms\n",
			s, (time_now() - global_start_walltime) / 1000.0, length,
			pa_bytes_to_usec(length, &mixer_sample_spec) / 1000.0);
		
		pa_usec_t latency = 0;
		int negative = 0;
		int result = pa_stream_get_latency(s, &latency, &negative);
		if (result == -PA_ERR_NODATA) {
			printf("  no stream latency data yet!\n");
		} else {
			printf("  stream latency: %.2lf ms\n", latency / 1000.0);
		}
		
		char buffer[512] = { 0 };
		pa_sample_spec_snprint(buffer, sizeof(buffer), pa_stream_get_sample_spec(s));
		printf("  sample_spec: %s\n", buffer);
		
		pa_operation* op = pa_stream_flush(s, NULL, NULL);
		pa_operation_unref(op);
		
		return;
	} else if (stream_data->bytes_in_mixer_buffer == -2) {
		stream_data->bytes_in_mixer_buffer = -3;
		
		printf("[%p] first packet after flush, after %.2lf ms, data: %zu bytes, %.2lf ms\n",
			s, (time_now() - global_start_walltime) / 1000.0, length,
			pa_bytes_to_usec(length, &mixer_sample_spec) / 1000.0);
		
		if ( ! init_stream_if_latency_known_or_drop_data() ) {
			return;
		}
	} else if (stream_data->bytes_in_mixer_buffer == -3) {
		printf("[%p] packet with unknown latency, after %.2lf ms, data: %zu bytes, %.2lf ms\n",
			s, (time_now() - global_start_walltime) / 1000.0, length,
			pa_bytes_to_usec(length, &mixer_sample_spec) / 1000.0);
		
		if ( ! init_stream_if_latency_known_or_drop_data() ) {
			return;
		}
	}
	
	//log_countdown = (log_countdown + 1) % 100;
	log_countdown = 0;
	if (!log_countdown) printf("[%p, %.2lf ms] %6zu bytes, %.2lf ms\n",
		s, stream_data->pts / 1000.0, length, pa_bytes_to_usec(length, &mixer_sample_spec) / 1000.0);
	
	while (pa_stream_readable_size(s) > 0) {
		// peek actually creates and fills the data vbl
		if (pa_stream_peek(s, &in_buffer_ptr, &in_buffer_size) < 0) {
			fprintf(stderr, "  Read failed\n");
			return;
		}
		
		if (in_buffer_ptr == NULL && in_buffer_size) {
			printf("  buffer is empty! no idea what to do.\n");
			continue;
		}
		
		if (in_buffer_ptr == NULL && in_buffer_size > 0) {
			printf("  hole of %zu bytes! don't add to mixer buffer.\n", in_buffer_size);
			stream_data->bytes_in_mixer_buffer += in_buffer_size;
			continue;
		}
		
		if (stream_data->bytes_in_mixer_buffer + in_buffer_size > mixer_buffer_size) {
			printf("  mixer buffer overflow from %s, droping audio packet\n", stream_data->name);
			pa_stream_drop(s);
			continue;
		}
		
		//printf("  %zu bytes\n", in_buffer_size);
		//pa_stream_write(audio_playback_stream, in_buffer_ptr, in_buffer_size, NULL, 0, PA_SEEK_RELATIVE);
		
		uint64_t start_pts = stream_data->pts;
		uint64_t end_pts = stream_data->pts + pa_bytes_to_usec(in_buffer_size, &mixer_sample_spec);
		
		const int16_t* in_samples_ptr = NULL;
		size_t in_sample_count = 0, in_samples_size = 0;;
		
		if (start_pts >= mixer_pts) {
			// The audio packet is newer than the stuff emitted by the mixer. So we can write
			// our entire audio into the mixer.
			in_samples_ptr  = in_buffer_ptr;
			in_samples_size = in_buffer_size;
			in_sample_count = in_samples_size / sizeof(in_samples_ptr[0]);
			printf("  writing %zu bytes into mixer\n", in_buffer_size);
		} else if (start_pts < mixer_pts && end_pts > mixer_pts) {
			// A part of the audio packet is to old but the rest is new stuff that should be
			// written into the mixer.
			size_t size_of_old_stuff = pa_usec_to_bytes(mixer_pts - start_pts, &mixer_sample_spec);
			in_samples_ptr  = in_buffer_ptr + size_of_old_stuff;
			in_samples_size = in_buffer_size - size_of_old_stuff;
			in_sample_count = in_samples_size / sizeof(in_samples_ptr[0]);
			printf("  skipping %zu bytes, writing %zu bytes into mixer (start %lu, end %lu, mixer %lu)\n",
				size_of_old_stuff, in_buffer_size - size_of_old_stuff, start_pts, end_pts, mixer_pts);
		} else {
			// This entire audio packet is way to old. The mixer already emitted newer audio
			// data. So throw this packet away.
			printf("  skipping %zu bytes (start %lu, end %lu, mixer %lu)\n",
				in_buffer_size, start_pts, end_pts, mixer_pts);
		}
		
		if (in_samples_ptr) {
			// Mix new sample into the mixer buffer
			int16_t* mixer_samples_ptr = mixer_buffer_ptr + stream_data->bytes_in_mixer_buffer;
			
			for(size_t i = 0; i < in_sample_count; i++) {
				//mixer_samples_ptr[i] = in_samples_ptr[i];
				
				int16_t a = mixer_samples_ptr[i], b = in_samples_ptr[i];
				if (a < 0 && b < 0)
					mixer_samples_ptr[i] = (a + b) - (a * b) / INT16_MIN;
				else if (a > 0 && b > 0)
					mixer_samples_ptr[i] = (a + b) - (a * b) / INT16_MAX;
				else
					mixer_samples_ptr[i] = a + b;
			}
			stream_data->bytes_in_mixer_buffer += in_samples_size;
		}
		
		stream_data->pts += pa_bytes_to_usec(in_buffer_size, &mixer_sample_spec);
		// swallow the data peeked at before
		pa_stream_drop(s);
	}
	
	// Check if a part of the mixer buffer has been written to by all streams. In that case
	// this part contains data from all streams and can be written out.
	if (!log_countdown) printf("  mixer %.2lf pts, bytes: ", mixer_pts / 1000.0);
	ssize_t min_bytes_in_mixer = INT32_MAX, max_bytes_in_mixer = 0;
	for(mic_p s = mics; s != NULL; s = s->next) {
		if (!log_countdown) printf("%4zd ", s->bytes_in_mixer_buffer);
		
		// Ignore streams that have not yet received any data
		if (s->bytes_in_mixer_buffer < 0)
			continue;
		
		if (s->bytes_in_mixer_buffer < min_bytes_in_mixer)
			min_bytes_in_mixer = s->bytes_in_mixer_buffer;
		if (s->bytes_in_mixer_buffer > max_bytes_in_mixer)
			max_bytes_in_mixer = s->bytes_in_mixer_buffer;
	}
	if (!log_countdown) printf("lag: %4zu bytes, %5.1f ms\n", max_bytes_in_mixer - min_bytes_in_mixer, pa_bytes_to_usec(max_bytes_in_mixer - min_bytes_in_mixer, &mixer_sample_spec) / 1000.0);
	
	if (min_bytes_in_mixer > 0) {
		pa_stream_write(audio_playback_stream, mixer_buffer_ptr, min_bytes_in_mixer, NULL, 0, PA_SEEK_RELATIVE);
		server_queue_frame(global_mainloop, 2, mixer_pts, mixer_buffer_ptr, min_bytes_in_mixer);
		
		mixer_pts += pa_bytes_to_usec(min_bytes_in_mixer, &mixer_sample_spec);
		
		size_t incomplete_mixer_bytes = max_bytes_in_mixer - min_bytes_in_mixer;
		if (incomplete_mixer_bytes > 0) {
			memmove(mixer_buffer_ptr, mixer_buffer_ptr + min_bytes_in_mixer, incomplete_mixer_bytes);
		}
		// Always clear the buffer space written out, even if no bytes are incomplete
		memset(mixer_buffer_ptr + incomplete_mixer_bytes, 0, max_bytes_in_mixer - incomplete_mixer_bytes);
		
		for(mic_p s = mics; s != NULL; s = s->next) {
			// Ignore streams that have not yet received any data
			if (s->bytes_in_mixer_buffer < 0)
				continue;
			
			s->bytes_in_mixer_buffer -= min_bytes_in_mixer;
		}
	}
}



//
// Server stuff
//
const char* socket_path = "server.sock";

typedef struct {
	int fd;
	pa_io_event* io_event;
	
	void* ptr;
	size_t size;
	
	list_node_p current_buffer_node;
} client_t, *client_p;

typedef struct {
	void*  ptr;
	size_t size;
	size_t refcount;
} buffer_t, *buffer_p;

int server_fd = -1;
list_p clients = NULL;
list_p buffers = NULL;

buffer_t header;

static void server_on_client_writable(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
static void buffer_node_unref(list_node_p buffer_node);


void mkv_build_header(uint16_t width, uint16_t height) {
	//webm_file = fopen("video-scratchpad/test.webm", "wb");
	FILE* f = open_memstream((char**)&header.ptr, &header.size);
	
	off_t o1, o2, o3, o4;
	
	o1 = ebml_element_start(f, MKV_EBML);
		ebml_element_string(f, MKV_DocType, "matroska");
	ebml_element_end(f, o1);
	
	ebml_element_start_unkown_data_size(f, MKV_Segment);
	
	o2 = ebml_element_start(f, MKV_Info);
		ebml_element_uint(f, MKV_TimecodeScale, 1000);  // specify timestamps in microseconds (usec)
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
				ebml_element_float(f, MKV_SamplingFrequency, mixer_sample_spec.rate);
				ebml_element_uint(f, MKV_Channels, mixer_sample_spec.channels);
				ebml_element_uint(f, MKV_BitDepth, pa_sample_size(&mixer_sample_spec) * 8);
			ebml_element_end(f, o4);
			
		ebml_element_end(f, o3);
		
	ebml_element_end(f, o2);
	
	fclose(f);
}

bool server_start(uint16_t width, uint16_t height) {
	server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (server_fd == -1)
		return perror("socket"), false;
	
	unlink(socket_path);
	
	struct sockaddr_un addr = { AF_UNIX, "" };
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	if ( bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1 )
		return perror("bind"), false;
	
	if ( listen(server_fd, 3) == -1 )
		return perror("listen"), false;
	
	clients = list_of(client_t);
	buffers = list_of(buffer_t);
	mkv_build_header(width, height);
	
	return true;
}

void server_close(pa_mainloop_api* mainloop) {
	for(list_node_p n = clients->first; n != NULL; n = n->next) {
		client_p client = list_value_ptr(n);
		
		close(client->fd);
		mainloop->io_free(client->io_event);
	}
	list_destroy(clients);
	close(server_fd);
	
	list_destroy(buffers);
	free(header.ptr);
}

static void server_accept_cb(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	int client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK);
	if (client_fd == -1) {
		perror("accept4");
		return;
	}
	
	client_p client = list_append_ptr(clients);
	list_node_p client_node = clients->last;
	
	client->fd = client_fd;
	client->io_event = mainloop->io_new(mainloop, client->fd, PA_IO_EVENT_OUTPUT, server_on_client_writable, client_node);
	
	client->ptr = header.ptr;
	client->size = header.size;
	client->current_buffer_node = NULL;
	
	printf("[client %d] connected\n", client->fd);
}

static void server_on_client_disconnect(pa_mainloop_api *mainloop, list_node_p client_node) {
	client_p client = list_value_ptr(client_node);
	
	printf("[client %d] disconnected\n", client->fd);
	
	// Cleanup this client
	close(client->fd);
	mainloop->io_free(client->io_event);
	
	// unref all buffers this client would've read
	if (client->current_buffer_node) {
		for (list_node_p node = client->current_buffer_node, next = NULL; node != NULL; node = next) {
			next = node->next;
			buffer_node_unref(node);
		}
	}
	
	list_remove(clients, client_node);
}

static void server_on_client_writable(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	client_p client = list_value_ptr(userdata);
	//printf("[client %d] writing data\n", client->fd);
	
	while (true) {
		ssize_t bytes_written = 0;
		while (client->size > 0) {
			bytes_written = write(client->fd, client->ptr, client->size);
			if (bytes_written < 0) {
				if (errno == EWOULDBLOCK) {
					// Very common case, do nothing
				} else if (errno == EPIPE) {
					server_on_client_disconnect(mainloop, userdata);
				} else {
					perror("write");
				}
				break;
			}
			
			client->ptr  += bytes_written;
			client->size -= bytes_written;
		}
		
		if (bytes_written >= 0) {
			// Buffer finished, switch to next one
			client->ptr = NULL;
			
			if (client->current_buffer_node != NULL) {
				list_node_p finished_buffer_node = client->current_buffer_node;
				client->current_buffer_node = client->current_buffer_node->next;
				buffer_node_unref(finished_buffer_node);
			}
			
			if (client->current_buffer_node != NULL) {
				// There is a next buffer ready. Switch this client to it.
				//printf("[client %d] switching to next buffer\n", client->fd);
				buffer_p next_buffer = list_value_ptr(client->current_buffer_node);
				client->ptr  = next_buffer->ptr;
				client->size = next_buffer->size;
			} else {
				// No next buffer, the client is stalled now. Disable it from the
				// mainloop since we don't have any data to write. We'll resume when
				// the next buffer comes around.
				//printf("[client %d] stalled\n", client->fd);
				mainloop->io_enable(client->io_event, PA_IO_EVENT_NULL);
				break;
			}
		} else {
			// Didn't finish buffer, continue next time we can write to client
			break;
		}
	}
}

static void buffer_node_unref(list_node_p buffer_node) {
	buffer_p buffer = list_value_ptr(buffer_node);
	buffer->refcount--;
	
	if (buffer->refcount == 0) {
		free(buffer->ptr);
		list_remove(buffers, buffer_node);
		//printf("[server] freeing buffer\n");
	}
}

void server_queue_frame(pa_mainloop_api *mainloop, uint8_t track, uint64_t timecode_us, void* frame_data, size_t frame_size) {
	size_t connected_client_count = list_count(clients);
	// Throw the buffer away if no one is listening
	if (connected_client_count == 0)
		return;
	
	//printf("[server] queuing frame\n");
	buffer_p buffer = list_append_ptr(buffers);
	buffer->refcount = connected_client_count;
	
	FILE* f = open_memstream((char**)&buffer->ptr, &buffer->size);
	
	off_t o2, o3;
	o2 = ebml_element_start(f, MKV_Cluster);
		ebml_element_uint(f, MKV_Timecode, timecode_us);
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
	
	// Check all clients and resume writing if necessary
	for(list_node_p n = clients->first; n != NULL; n = n->next) {
		client_p client = list_value_ptr(n);
		
		if (client->current_buffer_node == NULL) {
			// Switch client to the new buffer it it's stalled
			client->current_buffer_node = buffers->last;
			if (client->ptr == NULL) {
				client->ptr  = buffer->ptr;
				client->size = buffer->size;
			}
			
			// Enable this client in the mainloop
			//printf("[client %d] resuming\n", client->fd);
			mainloop->io_enable(client->io_event, PA_IO_EVENT_OUTPUT);
		}
	}
}
/*
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
				if (bytes_written < 0) {
					if (errno == EWOULDBLOCK) {
						// Very common case, do nothing
					} else if (errno == EPIPE) {
						printf("client %d disconnected\n", client_fd);
						close(client_fd);
						array_data(client_fds, int)[i] = -1;
					} else {
						perror("write");
					}
					break;
				}
				
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
	
	// Remove all disconnected clients (fd == -1)
	bool elem_func(array_p a, size_t index){
		return array_elem(a, int, index) == -1;
	}
	array_remove_func(client_fds, elem_func);
}
*/


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
	global_mainloop = mainloop;
	
	// Init signal handling
	int signal_fd = signals_init();
	
	// Init sound
	mixer_init(mainloop);
	
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
	
	
	// Setup sound input and output
	
	
	// Init local server
	server_start(cw, ch);
	
	
	// Start everything up
	for(size_t i = 0; i < video_input_count; i++)
		cam_stream_start(video_inputs[i].cam, 2);
	
	
	// Prepare mainloop event callbacks
	mainloop->io_new(mainloop, signal_fd, PA_IO_EVENT_INPUT, signals_cb, NULL);
	
	struct timeval next_sdl_check_time = usec_to_timeval( time_now() + 25000 );
	mainloop->time_new(mainloop, &next_sdl_check_time, sdl_event_check_cb, NULL);
	
	for(size_t i = 0; i < video_input_count; i++) {
		video_input_p vi = &video_inputs[i];
		pa_io_event* e = mainloop->io_new(mainloop, vi->cam->fd, PA_IO_EVENT_INPUT, camera_frame_cb, vi);
		//mainloop->io_enable(e, PA_IO_EVENT_NULL);
	}

	mainloop->io_new(mainloop, server_fd, PA_IO_EVENT_INPUT, server_accept_cb, NULL);
	
	// Do the loop
	global_start_walltime = time_now();
	while ( pa_mainloop_iterate(poll_mainloop, true, NULL) >= 0 ) {
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
				
				drawable_draw(stream);
				
			fbo_bind(NULL);
			
			fbo_read(stream_fbo, GL_RG, GL_UNSIGNED_BYTE, stream_video_ptr);
			
			uint64_t timecode = time_now() - global_start_walltime;
			server_queue_frame(mainloop, 1, timecode, stream_video_ptr, stream_video_size);
			
			glViewport(0, 0, ww, wh);
			
			drawable_draw(gui);
			
			SDL_GL_SwapWindow(win);
			
			something_to_render = false;
		}
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
	
	server_close(mainloop);
	
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
	
	drawable_destroy(stream);
	drawable_destroy(gui);
	drawable_destroy(video_on_composite);
	
	fbo_destroy(stream_fbo);
	fbo_destroy(composite_video);
	texture_destroy(composite_video_tex);
	free(stream_video_ptr);
	
	SDL_GL_DeleteContext(gl_ctx);
	SDL_DestroyWindow(win);
	
	mixer_cleanup();
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
	}
	
	// Restart the timer for the next time
	struct timeval next_sdl_check_time = usec_to_timeval( time_now() + 25000 );
	mainloop->time_restart(e, &next_sdl_check_time);
}

// Upload new video frames to the GPU
static void camera_frame_cb(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	video_input_p video_input = userdata;
	
	cam_buffer_t frame = cam_frame_get(video_input->cam);
		texture_update(video_input->tex, GL_RG, frame.ptr);
	cam_frame_release(video_input->cam);
	
	something_to_render = true;
}