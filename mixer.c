// For strdup()
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/pulseaudio.h>

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
	pa_stream* stream;
	uint8_t state;
	uint64_t pts;
	
	char* name;
	usec_t start;
	
	mic_p next;
};
mic_p mics = NULL;
pa_stream* audio_playback_stream = NULL;

#define MIC_STATE_WAITING_FOR_FIRST_PACKET   0
#define MIC_STATE_WAITING_FOR_KNOWN_LATENCY  1
#define MIC_STATE_MIXING                     2

void*    mixer_buffer_ptr = NULL;
size_t   mixer_buffer_size = 0;
size_t   mixer_pos = 0;
uint64_t mixer_pts = 0;

pa_context* context = NULL;
uint16_t log_countdown = 0;
static usec_t global_start_walltime = 0;

static void mixer_on_context_state_changed(pa_context *c, void *userdata);
static void source_info_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static void on_new_mic_data(pa_stream *s, size_t length, void *userdata);
static void playback_audio(void* buffer_ptr, size_t buffer_size);


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

void mixer_output_peek(void** buffer_ptr, size_t* buffer_size, uint64_t* buffer_pts) {
	*buffer_ptr  = mixer_buffer_ptr;
	*buffer_size = mixer_pos;
	*buffer_pts  = mixer_pts - pa_bytes_to_usec(mixer_pos, &mixer_sample_spec);
}

void mixer_output_consume() {
	size_t remaining_size = mixer_buffer_size - mixer_pos;
	// Move everything after the mixer pos to the start of the mixer buffer
	memmove(mixer_buffer_ptr, mixer_buffer_ptr + mixer_pos, remaining_size);
	// Zero the now unused buffer area so it's safe to mix new data onto it
	memset(mixer_buffer_ptr + remaining_size, 0, mixer_pos);
	
	mixer_pos = 0;
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
	mic->state = MIC_STATE_WAITING_FOR_FIRST_PACKET;
	mic->stream = pa_stream_new(c, "HDswitch", &mixer_sample_spec, NULL);
	
	mic->name = strdup(i->description);
	
	mic->next = mics;
	mics = mic;
	
	pa_stream_set_read_callback(mic->stream, on_new_mic_data, mic);
	
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


static void on_new_mic_data(pa_stream *s, size_t length, void *userdata) {
	mic_p mic = userdata;
	
	void print_packet_details(const char* description) {
		printf("[mic %15.15s] %s after %.2lf ms, data: %zu bytes, %.2lf ms, latency: ",
			mic->name, description, (time_now() - global_start_walltime) / 1000.0,
			length, pa_bytes_to_usec(length, &mixer_sample_spec) / 1000.0);
		
		pa_usec_t latency = 0;
		int negative = 0;
		int result = pa_stream_get_latency(s, &latency, &negative);
		if (result == -PA_ERR_NODATA) {
			printf("unknown\n");
		} else {
			printf("%.2lf ms\n", latency / 1000.0);
		}
	}
	
	
	if (mic->state == MIC_STATE_WAITING_FOR_FIRST_PACKET) {
		// Flush the stream as soon as the first packet comes around. Sometimes it can take
		// several seconds (!) to initialize the mic and once the data flows it can be quite
		// old. E.g. a mic takes 6 seconds until we get the first packet and Pulse Audio
		// has 4 seconds of audio data in it's buffers. In that case we would start of with
		// a 4 second latency! To avoid that the flush the stream on the first packet. In the
		// example this kills the 4 seconds of buffered data and we would get up to date data
		// on the next packet.
		print_packet_details("initial packet");
		
		pa_operation* op = pa_stream_flush(s, NULL, NULL);
		pa_operation_unref(op);
		
		mic->state = MIC_STATE_WAITING_FOR_KNOWN_LATENCY;
		return;
	} else if (mic->state == MIC_STATE_WAITING_FOR_KNOWN_LATENCY) {
		// No we get decent up to date audio data. But to really get the proper timing we still
		// need to know the latency of the data. That is how many milliseconds ago the current
		// audio packet was recorded.
		// For the first few packets this can be unknown since Pulse Audio doesn't have any
		// timing data yet. We throw these away because we can't accuratly place them on the
		// mixer "timeline".
		// As soon as we get a packet with known latency we know where to place the data on the
		// timeline and can initialize the stream properly.
		// If the stream latency is way to high (e.g. really bad or broken mic) just ignore
		// the latency and mix the data right in when it comes around. If it's important the user
		// can identify the mic and fix it. But we don't block all other mics because of one
		// broken mic.
		print_packet_details("after flush packet");
		
		pa_usec_t latency = 0;
		int negative = 0;
		int result = pa_stream_get_latency(s, &latency, &negative);
		
		if (result != -PA_ERR_NODATA) {
			mic->pts = time_now() - latency - global_start_walltime;
			printf("  stream latency: %.2lf ms, pts: %.2lf ms\n", latency / 1000.0, mic->pts / 1000.0);
			
			// Initialize the mixer PTS if no one is using the mixer yet
			if (mixer_pts == 0)
				mixer_pts = mic->pts;
			
			// If the stream latency is above the mixer block threshold don't make
			// the mixer wait (block) for this stream but instead mix it as we get it.
			// This way the user might be able to detect the faulty mic with to much latency.
			if (latency / 1000 > max_latency_for_mixer_block_ms) {
				mic->pts = mixer_pts;
				printf("  IGNORING MIC LATENCY because it's to high! We don't want everything to wait for it.\n");
			}
			
			// Stream timing is properly setup now, so continue on and mix the streams audio
			// data into the mixer buffer.
			// The timing data is for the current audio packet so also mix this one in. Therefore
			// no return statement here, we want to continue with the code further down!
			mic->state = MIC_STATE_MIXING;
		} else {
			// Don't know the latency of the data so no idea where it belongs on the
			// timeline. So throw it away and wait for the next packet.
			const void *in_buffer_ptr;
			size_t in_buffer_size;
			
			while (pa_stream_readable_size(s) > 0) {
				if (pa_stream_peek(s, &in_buffer_ptr, &in_buffer_size) < 0) {
					fprintf(stderr, "  Read failed\n");
					return;
				}
				pa_stream_drop(s);
			}
			
			return;
		}
	}
	
	
	bool log_packets = true;
	if (log_packets) printf("[mic %15.15s pts: %.2lf ms] %6zu bytes, %.2lf ms\n",
			mic->name, mic->pts / 1000.0,
			length, pa_bytes_to_usec(length, &mixer_sample_spec) / 1000.0);
	
	// Read all the audio data from the packet and mix it into the mixer buffer
	uint64_t mixer_buffer_end_pts = mixer_pts + pa_bytes_to_usec(mixer_buffer_size - mixer_pos, &mixer_sample_spec);
	while (pa_stream_readable_size(s) > 0) {
		// Read the audio data and make sure we have enough space for it in the mixer
		const void *in_buffer_ptr;
		size_t in_buffer_size;
		
		if (pa_stream_peek(s, &in_buffer_ptr, &in_buffer_size) < 0) {
			fprintf(stderr, "pa_stream_peek(): read failed\n");
			return;
		}
		
		if (in_buffer_ptr == NULL && in_buffer_size == 0) {
			if (log_packets) printf("  buffer is empty! no idea what to do.\n");
			continue;
		}
		
		uint64_t packet_start_pts = mic->pts;
		uint64_t packet_duration = pa_bytes_to_usec(in_buffer_size, &mixer_sample_spec);
		uint64_t packet_end_pts = packet_start_pts + packet_duration;
		
		if (in_buffer_ptr == NULL && in_buffer_size > 0) {
			if (log_packets) printf("  hole of %zu bytes! skip ahead in mixer buffer.\n", in_buffer_size);
			mic->pts += packet_duration;
			continue;
		}
		
		if (packet_end_pts > mixer_buffer_end_pts) {
			if (log_packets) printf("  mixer buffer overflow, droping audio packet\n");
			mic->pts += packet_duration;
			pa_stream_drop(s);
			continue;
		}
		
		// Determine what part of the incomming audio data is new enough so we can write it
		// into the mixer buffer.
		const int16_t* in_samples_ptr = NULL;
		size_t in_sample_count = 0, in_samples_size = 0;
		uint64_t in_samples_pts = 0;
		
		if (packet_start_pts >= mixer_pts) {
			// The audio packet is newer than the stuff emitted by the mixer. So we can write
			// our entire audio into the mixer.
			in_samples_ptr  = in_buffer_ptr;
			in_samples_size = in_buffer_size;
			in_sample_count = in_samples_size / sizeof(in_samples_ptr[0]);
			in_samples_pts  = packet_start_pts;
			if (log_packets) printf("  writing %zu bytes into mixer\n", in_samples_size);
		} else if (packet_start_pts < mixer_pts && packet_end_pts > mixer_pts) {
			// A part of the audio packet is to old but the rest is new stuff that should be
			// written into the mixer.
			size_t size_of_old_stuff = pa_usec_to_bytes(mixer_pts - packet_start_pts, &mixer_sample_spec);
			in_samples_ptr  = in_buffer_ptr + size_of_old_stuff;
			in_samples_size = in_buffer_size - size_of_old_stuff;
			in_sample_count = in_samples_size / sizeof(in_samples_ptr[0]);
			in_samples_pts  = mixer_pts;
			if (log_packets) printf("  skipping %zu bytes, writing %zu bytes into mixer (pts: start %lu, end %lu, mixer %lu)\n",
				size_of_old_stuff, in_samples_size, packet_start_pts, packet_end_pts, mixer_pts);
		} else {
			// This entire audio packet is to old. The mixer already emitted newer audio
			// data. So throw this packet away.
			if (log_packets) printf("  skipping %zu bytes (pts: start %lu, end %lu, mixer %lu)\n",
				in_buffer_size, packet_start_pts, packet_end_pts, mixer_pts);
		}
		
		if (in_samples_ptr) {
			// Mix new samples into the mixer buffer
			int16_t* mixer_samples_ptr = mixer_buffer_ptr + mixer_pos + pa_usec_to_bytes(in_samples_pts - mixer_pts, &mixer_sample_spec);
			//int16_t* mixer_samples_ptr = mixer_buffer_ptr + stream_data->bytes_in_mixer_buffer;
			
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
		}
		
		// Advance the mics PTS and drop the consumed audio data
		mic->pts += packet_duration;
		pa_stream_drop(s);
	}
	
	// Check if a part of the mixer buffer has been written to by all streams. In that case
	// this part contains data from all streams and can be written out.
	if (log_packets) printf("  mixer pts %.2lf ms, stream pts: ", mixer_pts / 1000.0);
	uint64_t min_stream_pts = UINT64_MAX, max_stream_pts = 0;
	for(mic_p m = mics; m != NULL; m = m->next) {
		// Ignore streams that do not yet mix data into the mixer buffer
		if (m->state != MIC_STATE_MIXING)
			continue;
		
		uint64_t pts = m->pts;
		if (log_packets) printf("%.2lf ", pts / 1000.0);
		
		// If a new stream needs to catch up with the mixer its pts is smaller than the
		// mixer pts. Set it to the mixer pts for the min/max calculation so it stalls
		// the mixer until it cought up without causing a fatal overflow in the
		// finished_duration variable fruther down.
		if (pts < mixer_pts)
			pts = mixer_pts;
		
		if (pts < min_stream_pts)
			min_stream_pts = pts;
		if (pts > max_stream_pts)
			max_stream_pts = pts;
	}
	uint64_t finished_duration = min_stream_pts - mixer_pts;
	uint64_t incomplete_duration = max_stream_pts - min_stream_pts;
	if (log_packets) printf("finished: %.2lf ms, incomplete: %.2lf ms\n",
		finished_duration / 1000.0, incomplete_duration / 1000.0);
	
	if (mixer_pts + finished_duration > mixer_buffer_end_pts) {
		// There is no mixer buffer space left but the streams presentation timestamps (PTS)
		// went ahead. So we have a finished buffer area that is larger than the rest of the
		// buffer. In that case we can't do much but hope that someone consumes the pending
		// mixer buffer data so we get more free space again.
	} else if (finished_duration > 0) {
		// We actually got a finished part of the mixer buffer. Play it back immediately
		// and advance our mixer position. The mixer buffer area before the mixer position
		// needs to be consumed by mixer_output_consume() before we can overwrite it.
		size_t finished_size = pa_usec_to_bytes(finished_duration, &mixer_sample_spec);
		
		playback_audio(mixer_buffer_ptr + mixer_pos, finished_size);
		
		mixer_pos += finished_size;
		mixer_pts += finished_duration;
	}
	/*
	if (min_bytes_in_mixer > 0) {
		playback_audio(mixer_buffer_ptr, min_bytes_in_mixer);
		
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
	*/
}


//
// Utility functions
//

static void playback_audio(void* buffer_ptr, size_t buffer_size) {
	if (audio_playback_stream == NULL) {
		audio_playback_stream = pa_stream_new(context, "HDswitch", &mixer_sample_spec, NULL);
		
		pa_buffer_attr buffer_attr = { 0 };
		buffer_attr.maxlength = (uint32_t) -1;
		buffer_attr.prebuf = (uint32_t) -1;
		buffer_attr.fragsize = (uint32_t) -1;
		buffer_attr.tlength = pa_usec_to_bytes(latency_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
		buffer_attr.minreq = (uint32_t) -1;
		
		pa_stream_connect_playback(audio_playback_stream, NULL, &buffer_attr, PA_STREAM_ADJUST_LATENCY, NULL, NULL);
	}
	
	pa_stream_write(audio_playback_stream, buffer_ptr, buffer_size, NULL, 0, PA_SEEK_RELATIVE);
}