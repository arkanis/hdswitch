// for strdup
#define _BSD_SOURCE
#define _POSIX_SOURCE
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pulse/pulseaudio.h>


// Config variables
const uint32_t latency_ms = 10;
const uint32_t mixer_buffer_time_ms = 1000;


static void signals_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
static void context_state_cb(pa_context *c, void *userdata);
static void source_info_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static void stream_read_cb(pa_stream *s, size_t length, void *userdata);
static int signals_init();
static int signals_cleanup(int signal_fd);


pa_mainloop* pa_ml = NULL;
pa_context* context = NULL;

typedef struct stream_s stream_t, *stream_p;
struct stream_s {
	ssize_t bytes_in_mixer_buffer;
	pa_stream* stream;
	char* name;
	stream_p next;
};
stream_p streams = NULL;
pa_stream* out_stream = NULL;


pa_sample_spec mixer_sample_spec = {
	.format   = PA_SAMPLE_S16LE,
	.rate     = 48000,
	.channels = 2
};

void*    mixer_buffer_ptr = NULL;
size_t   mixer_buffer_size = 0;
uint64_t mixer_pts = 0;


int main() {
	int signal_fd = signals_init();
	
	mixer_buffer_size = pa_usec_to_bytes(mixer_buffer_time_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
	mixer_buffer_ptr = malloc(mixer_buffer_size);
	memset(mixer_buffer_ptr, 0, mixer_buffer_size);
	
	pa_ml = pa_mainloop_new();
	pa_mainloop_api* pa_api = pa_mainloop_get_api(pa_ml);
	context = pa_context_new(pa_api, "HDswitch");
	
	pa_context_set_state_callback(context, context_state_cb, NULL);
	pa_context_connect(context, NULL, 0, NULL);
	
	pa_api->io_new(pa_api, signal_fd, PA_IO_EVENT_INPUT, signals_cb, NULL);
	int dispached_events = 0;
	while ( (dispached_events = pa_mainloop_iterate(pa_ml, true, NULL)) >= 0 ) {
		//printf("dispatched %d events\n", dispached_events);
	}
	
	free(mixer_buffer_ptr);
	pa_context_disconnect(context);
	signals_cleanup(signal_fd);
	
	return 0;
}





static void context_state_cb(pa_context *c, void *userdata) {
	switch (pa_context_get_state(c)) {
		case PA_CONTEXT_UNCONNECTED:  printf("PA_CONTEXT_UNCONNECTED\n");  break;
		case PA_CONTEXT_CONNECTING:   printf("PA_CONTEXT_CONNECTING\n");   break;
		case PA_CONTEXT_AUTHORIZING:  printf("PA_CONTEXT_AUTHORIZING\n");  break;
		case PA_CONTEXT_SETTING_NAME: printf("PA_CONTEXT_SETTING_NAME\n"); break;
		case PA_CONTEXT_READY:        printf("PA_CONTEXT_READY\n");
			pa_context_get_source_info_list(c, source_info_list_cb, NULL);
			break;
		case PA_CONTEXT_FAILED:       printf("PA_CONTEXT_FAILED\n");     break;
		case PA_CONTEXT_TERMINATED:   printf("PA_CONTEXT_TERMINATED\n"); break;
		default: break;
	}
	
	return;
}

static void source_info_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
	// Source list finished, create the playback stream
	if (i == NULL) {
		out_stream = pa_stream_new(c, "HDswitch", &mixer_sample_spec, NULL);
		
		pa_buffer_attr buffer_attr = { 0 };
		buffer_attr.maxlength = (uint32_t) -1;
		buffer_attr.prebuf = (uint32_t) -1;
		buffer_attr.fragsize = (uint32_t) -1;
		buffer_attr.tlength = pa_usec_to_bytes(latency_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
		buffer_attr.minreq = (uint32_t) -1;
		
		pa_stream_connect_playback(out_stream, NULL, &buffer_attr, PA_STREAM_ADJUST_LATENCY, NULL, NULL);
		return;
	}
	
	// Ignore all monitor streams, we only want real mics
	if ( !(i->flags & PA_SOURCE_HARDWARE) )
		return;
	
	/* use to only record the first stream
	if (streams != NULL)
		return;
	*/
	
	printf("name: %s\n", i->name);
	printf("index: %u\n", i->index);
	printf("description: %s\n", i->description);
	printf("driver: %s\n", i->driver);
	printf("latency: %.1f ms, configured: %.1f ms\n", i->latency / 1000.0, i->configured_latency / 1000.0);
	
	char buffer[512] = { 0 };
	pa_sample_spec_snprint(buffer, sizeof(buffer), &i->sample_spec);
	printf("sample_spec: %s\n", buffer);
	
	pa_channel_map_snprint(buffer, sizeof(buffer), &i->channel_map);
	printf("channel_map: %s\n", buffer);
	
	printf("flags:\n");
	if (i->flags & PA_SOURCE_HW_VOLUME_CTRL)  printf("- PA_SOURCE_HW_VOLUME_CTRL\n");
	if (i->flags & PA_SOURCE_LATENCY)         printf("- PA_SOURCE_LATENCY\n");
	if (i->flags & PA_SOURCE_HARDWARE)        printf("- PA_SOURCE_HARDWARE\n");
	if (i->flags & PA_SOURCE_NETWORK)         printf("- PA_SOURCE_NETWORK\n");
	if (i->flags & PA_SOURCE_HW_MUTE_CTRL)    printf("- PA_SOURCE_HW_MUTE_CTRL\n");
	if (i->flags & PA_SOURCE_DECIBEL_VOLUME)  printf("- PA_SOURCE_DECIBEL_VOLUME\n");
	if (i->flags & PA_SOURCE_DYNAMIC_LATENCY) printf("- PA_SOURCE_DYNAMIC_LATENCY\n");
	if (i->flags & PA_SOURCE_FLAT_VOLUME)     printf("- PA_SOURCE_FLAT_VOLUME\n");
	
	printf("properties:\n");
	const char* key = NULL;
	void* it = NULL;
	while ( (key = pa_proplist_iterate(i->proplist, &it)) != NULL ) {
		printf("- %s: %s\n", key, pa_proplist_gets(i->proplist, key));
	}
	
	pa_stream* stream = pa_stream_new(c, "HDswitch", &mixer_sample_spec, NULL);
	
	stream_p stream_data = malloc(sizeof(stream_t));
	stream_data->bytes_in_mixer_buffer = -1;
	stream_data->next = streams;
	stream_data->stream = stream;
	stream_data->name = strdup(i->description);
	streams = stream_data;
	pa_stream_set_read_callback(stream, stream_read_cb, stream_data);
	
	pa_buffer_attr buffer_attr = { 0 };
	buffer_attr.maxlength = (uint32_t) -1;
	buffer_attr.prebuf = (uint32_t) -1;
	buffer_attr.fragsize = pa_usec_to_bytes(latency_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
	buffer_attr.tlength = (uint32_t) -1;
	buffer_attr.minreq = (uint32_t) -1;
	
	if ( pa_stream_connect_record(stream, i->name, &buffer_attr, PA_STREAM_ADJUST_LATENCY) < 0 ) {
		printf("pa_stream_connect_record() failed: %s", pa_strerror(pa_context_errno(c)));
	}
}

static void stream_read_cb(pa_stream *s, size_t length, void *userdata) {
	const void *in_buffer_ptr;
	size_t in_buffer_size;
	stream_p stream_data = userdata;
	
	// Initialize the byte offset only as soon as we got the first data packet.
	// Otherwise always waits for this stream from it's start to the arival of the first packet.
	if (stream_data->bytes_in_mixer_buffer == -1)
		stream_data->bytes_in_mixer_buffer = 0;
	
	printf("stream_read_cb, stream %p, %6zu bytes\n", s, length);
	
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
			printf("  mixer buffer overflow from %s, retrying audio package next time\n", stream_data->name);
			break;
		}
		
		// Mix new sample into the mixer buffer
		const int16_t* in_samples_ptr = in_buffer_ptr;
		size_t in_sample_count = in_buffer_size / sizeof(in_samples_ptr[0]);
		int16_t* mixer_samples_ptr = mixer_buffer_ptr + stream_data->bytes_in_mixer_buffer;
		
		for(size_t i = 0; i < in_sample_count; i++) {
			int16_t a = mixer_samples_ptr[i], b = in_samples_ptr[i];
			if (a < 0 && b < 0)
				mixer_samples_ptr[i] = (a + b) - (a * b) / INT16_MIN;
			else if (a > 0 && b > 0)
				mixer_samples_ptr[i] = (a + b) - (a * b) / INT16_MAX;
			else
				mixer_samples_ptr[i] = a + b;
		}
		stream_data->bytes_in_mixer_buffer += in_buffer_size;
		
		// swallow the data peeked at before
		pa_stream_drop(s);
	}
	
	// Check if a part of the mixer buffer has been written to by all streams. In that case
	// this part contains data from all streams and can be written out.
	printf("  mixer bytes: ");
	ssize_t min_bytes_in_mixer = INT32_MAX, max_bytes_in_mixer = 0;
	for(stream_p s = streams; s != NULL; s = s->next) {
		printf("%4zd ", s->bytes_in_mixer_buffer);
		
		// Ignore streams that have not yet received any data
		if (s->bytes_in_mixer_buffer == -1)
			continue;
		
		if (s->bytes_in_mixer_buffer < min_bytes_in_mixer)
			min_bytes_in_mixer = s->bytes_in_mixer_buffer;
		if (s->bytes_in_mixer_buffer > max_bytes_in_mixer)
			max_bytes_in_mixer = s->bytes_in_mixer_buffer;
	}
	printf("lag: %4zu bytes, %.1f ms\n", max_bytes_in_mixer - min_bytes_in_mixer, pa_bytes_to_usec(max_bytes_in_mixer - min_bytes_in_mixer, &mixer_sample_spec) / 1000.0);
	
	if (min_bytes_in_mixer > 0) {
		pa_stream_write(out_stream, mixer_buffer_ptr, min_bytes_in_mixer, NULL, 0, PA_SEEK_RELATIVE);
		
		size_t incomplete_mixer_bytes = max_bytes_in_mixer - min_bytes_in_mixer;
		if (incomplete_mixer_bytes > 0) {
			memmove(mixer_buffer_ptr, mixer_buffer_ptr + min_bytes_in_mixer, incomplete_mixer_bytes);
			memset(mixer_buffer_ptr + incomplete_mixer_bytes, 0, max_bytes_in_mixer - incomplete_mixer_bytes);
		}
		
		for(stream_p s = streams; s != NULL; s = s->next) {
			// Ignore streams that have not yet received any data
			if (s->bytes_in_mixer_buffer == -1)
				continue;
			
			s->bytes_in_mixer_buffer -= min_bytes_in_mixer;
		}
	}
}


//
// Signal handling
//

static int signals_init() {
	// Setup SIGINT and SIGTERM to terminate our poll loop. For that we read them via a signal fd.
	// To prevent the signals from interrupting our process we need to block them first.
	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGINT);
	sigaddset(&signal_mask, SIGTERM);
	
	if ( sigprocmask(SIG_BLOCK, &signal_mask, NULL) == -1 )
		return perror("sigprocmask"), -1;
	
	int signals = signalfd(-1, &signal_mask, SFD_NONBLOCK);
	if (signals == -1)
		return perror("signalfd"), -1;
	
	return signals;
}

static void signals_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	struct signalfd_siginfo siginfo = {0};
	if ( read(fd, &siginfo, sizeof(siginfo)) > 0 ) {
		printf("got signal %u from pid %u\n", siginfo.ssi_signo, siginfo.ssi_pid);
		
		for(stream_p s = streams; s != NULL; s = s->next) {
			pa_stream_disconnect(s->stream);
			pa_stream_unref(s->stream);
			s->stream = NULL;
		}
		
		pa_mainloop_quit(pa_ml, 0);
	} else {
		perror("read(signalfd)");
	}
}

static int signals_cleanup(int signal_fd) {
	close(signal_fd);
	
	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGINT);
	sigaddset(&signal_mask, SIGTERM);
	if ( sigprocmask(SIG_UNBLOCK, &signal_mask, NULL) == -1 )
		return perror("sigprocmask"), -1;
	
	return 1;
}