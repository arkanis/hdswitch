// For strdup()
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/pulseaudio.h>

#include "server.h"
#include "timer.h"
#include "mixer.h"


//
// Mixer stuff
//

// Config variables
uint32_t latency_ms = 0;
uint32_t mixer_buffer_time_ms = 0;
uint32_t max_latency_for_mixer_block_ms = 0;
pa_sample_spec mixer_sample_spec;

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


void*    mixer_buffer_ptr = NULL;
size_t   mixer_buffer_size = 0;
uint64_t mixer_pts = 0;

pa_context* context = NULL;
uint16_t log_countdown = 0;
static usec_t global_start_walltime = 0;

static void mixer_on_context_state_changed(pa_context *c, void *userdata);
static void source_info_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static void mixer_on_new_mic_data(pa_stream *s, size_t length, void *userdata);


bool mixer_start(usec_t start_walltime, uint32_t requested_latency_ms, uint32_t buffer_time_ms, uint32_t max_latency_to_block_for_ms, pa_sample_spec sample_spec, pa_mainloop_api* mainloop) {
	latency_ms = requested_latency_ms;
	mixer_buffer_time_ms = buffer_time_ms;
	max_latency_for_mixer_block_ms = max_latency_to_block_for_ms;
	
	global_start_walltime = start_walltime;
	mixer_sample_spec = sample_spec;
	
	mixer_buffer_size = pa_usec_to_bytes(mixer_buffer_time_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
	mixer_buffer_ptr = malloc(mixer_buffer_size);
	memset(mixer_buffer_ptr, 0, mixer_buffer_size);
	
	context = pa_context_new(mainloop, "HDswitch");
	pa_context_set_state_callback(context, mixer_on_context_state_changed, NULL);
	pa_context_connect(context, NULL, 0, NULL);
	
	return true;
}

void mixer_stop() {
	free(mixer_buffer_ptr);
	pa_context_disconnect(context);
}


//
// Event handlers
//

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
		server_enqueue_frame(2, mixer_pts, mixer_buffer_ptr, min_bytes_in_mixer);
		
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