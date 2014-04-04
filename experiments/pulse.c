#define _POSIX_SOURCE
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pulse/pulseaudio.h>


static void signals_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
static void context_state_cb(pa_context *c, void *userdata);
static void source_info_list_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static void stream_read_cb(pa_stream *s, size_t length, void *userdata);
static int signals_init();
static int signals_cleanup(int signal_fd);

const uint32_t latency_ms = 200;
const uint32_t mixer_buffer_time_ms = 100;

pa_mainloop* pa_ml = NULL;
pa_context* context = NULL;

typedef struct stream_s stream_t, *stream_p;
struct stream_s {
	size_t mixer_offset;
	pa_stream* stream;
	stream_p next;
};
stream_p streams = NULL;
pa_stream* out_stream = NULL;


pa_sample_spec mixer_sample_spec = {
	.format   = PA_SAMPLE_S16LE,
	.rate     = 48000,
	.channels = 2
};
int16_t mixer_buffer_ptr = NULL;
size_t   mixer_buffer_size = 0;
uint64_t mixer_pts = 0;


int main() {
	int signal_fd = signals_init();
	
	mixer_buffer_size = pa_usec_to_bytes(mixer_buffer_time_ms * PA_USEC_PER_MSEC, &mixer_sample_spec);
	mixer_buffer_ptr = malloc(mixer_buffer_size);
	
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



static void signals_cb(pa_mainloop_api *ea, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	struct signalfd_siginfo siginfo = {0};
	if ( read(fd, &siginfo, sizeof(siginfo)) > 0 ) {
		printf("got signal %u from pid %u\n",
			siginfo.ssi_signo, siginfo.ssi_pid);
		pa_mainloop_quit(pa_ml, 0);
	} else {
		printf("error while retrieving signal\n");
	}
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
	
	if ( !(i->flags & PA_SOURCE_HARDWARE) )
		return;
	
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
	stream_data->mixer_offset = 0;
	stream_data->next = streams;
	stream_data->stream = stream;
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
	const void *data_ptr;
	size_t data_size;
	stream_p stream_data = userdata;
	const pa_sample_spec* sample_spec = pa_stream_get_sample_spec(s);
	
	while (pa_stream_readable_size(s) > 0) {
		// peek actually creates and fills the data vbl
		if (pa_stream_peek(s, &data_ptr, &data_size) < 0) {
			fprintf(stderr, "Read failed\n");
			return;
		}
		
		memcpy(mixer_buffer_ptr + stream_data->mixer_offset, data_ptr, data_size);
		stream_data->mixer_offset += data_size;
		
		/*
		fprintf(stderr, "Stream %p: got %4zu bytes, %5.2f ms       ", s, data_size, pa_bytes_to_usec(data_size, sample_spec) / 1000.0);
		size_t i = 0;
		for(stream_p it = streams; it != NULL; it = it->next) {
			const pa_sample_spec* ss = pa_stream_get_sample_spec(it->stream);
			fprintf(stderr, "%zu: %5.3f  ", i, it->pts / (float)ss->rate);
			i++;
		}
		fprintf(stderr, "\n");
		*/
		
		pa_stream_write(out_stream, data_ptr, data_size, NULL, 0, PA_SEEK_RELATIVE);
		
		// swallow the data peeked at before
		pa_stream_drop(s);
	}
}



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