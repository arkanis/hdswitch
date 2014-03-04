#define _POSIX_SOURCE
#include <signal.h>
#include <sys/signalfd.h>

#include <stdbool.h>
#include <stdint.h>
#include <alloca.h>
#include <errno.h>
#include <poll.h>



#include "../timer.h"

// Prevent ALSA from defining struct timeval by itself. Clashes with timer.h
#define _POSIX_C_SOURCE
#include <alsa/asoundlib.h>

#define error_checked(func, message) if((func) < 0){ printf(message); return; }

typedef struct {
	snd_pcm_t* pcm;
	uint32_t   rate;
	uint32_t   channels;
	size_t     sample_size;
	size_t     buffer_size;
} sound_t, *sound_p;

sound_p sound_open(const char* name, snd_pcm_stream_t type, int flags);
void    sound_configure(sound_p sound, uint32_t rate, uint32_t channels, snd_pcm_format_t format, bool interleaved, size_t period_time_ms);
void    sound_close(sound_p sound);
size_t  sound_poll_fds_count(sound_p sound);
size_t  sound_poll_fds(sound_p sound, struct pollfd* fds, size_t nfds);
uint16_t sound_poll_fds_revents(sound_p sound, struct pollfd* fds, size_t nfds);
ssize_t sound_read(sound_p sound, void* buffer, size_t size);
ssize_t sound_write(sound_p sound, const void* buffer, size_t size);
ssize_t sound_recover(sound_p sound, ssize_t error);


sound_p sound_open(const char* name, snd_pcm_stream_t type, int flags) {
	sound_p sound = malloc(sizeof(sound_t));
	memset(sound, 0, sizeof(sound_t));
	
	snd_pcm_open(&sound->pcm, name, type, flags);
	
	return sound;
}

void sound_close(sound_p sound) {
	snd_pcm_close(sound->pcm);
	memset(sound, 0, sizeof(sound_t));
	free(sound);
}


void sound_configure(sound_p sound, uint32_t rate, uint32_t channels, snd_pcm_format_t format, bool interleaved, size_t period_time_ms) {
	snd_pcm_hw_params_t* params = NULL;
	
	snd_pcm_hw_params_alloca(&params);
	error_checked( snd_pcm_hw_params_any(sound->pcm, params), "Can not configure this PCM device\n" );
	
	sound->rate = rate;
	error_checked( snd_pcm_hw_params_set_rate_near(sound->pcm, params, &sound->rate, 0), "Error setting rate.\n");
	printf("rate: %d\n", rate);
	
	sound->channels = channels;
	error_checked( snd_pcm_hw_params_set_channels(sound->pcm, params, sound->channels), "Error setting channels.\n");
	
	error_checked( snd_pcm_hw_params_set_format(sound->pcm, params, format), "Error setting format\n");
	error_checked( snd_pcm_hw_params_set_access(sound->pcm, params, interleaved ? SND_PCM_ACCESS_RW_INTERLEAVED : SND_PCM_ACCESS_RW_NONINTERLEAVED), "Error setting access\n");
	
	
	uint32_t period_time_us = period_time_ms*1000, periods = 2;
	error_checked( snd_pcm_hw_params_set_period_time_near(sound->pcm, params, &period_time_us, NULL), "Error setting period time\n");
	error_checked( snd_pcm_hw_params_set_periods_near(sound->pcm, params, &periods, NULL), "Error setting periods.\n");
	printf("period time: %.3fms, %d periods\n", period_time_us / 1000.0f, periods);
	
	snd_pcm_uframes_t buffer_size_in_frames = 0;
	uint32_t buffer_time_us = 0;
	error_checked( snd_pcm_hw_params_get_buffer_time(params, &buffer_time_us, NULL), "Error getting buffer time\n");
	error_checked( snd_pcm_hw_params_get_buffer_size(params, &buffer_size_in_frames), "Error getting buffer size\n");
	
	ssize_t bytes_per_frame = snd_pcm_format_size(format, channels);
	sound->buffer_size = buffer_size_in_frames * bytes_per_frame;
	
	printf("buffer size: %lu bytes, buffer time: %.3fms\n", sound->buffer_size, buffer_time_us / 1000.0f);
	
	error_checked( snd_pcm_hw_params(sound->pcm, params), "Error setting HW params.\n" );
	
	sound->sample_size = snd_pcm_samples_to_bytes(sound->pcm, 1);
}

size_t sound_poll_fds_count(sound_p sound) {
	return snd_pcm_poll_descriptors_count(sound->pcm);
}

size_t sound_poll_fds(sound_p sound, struct pollfd* fds, size_t nfds) {
	return snd_pcm_poll_descriptors(sound->pcm, fds, nfds);
}

uint16_t sound_poll_fds_revents(sound_p sound, struct pollfd* fds, size_t nfds) {
	uint16_t revents = 0;
	snd_pcm_poll_descriptors_revents(sound->pcm, fds, nfds, &revents);
	return revents;
}

ssize_t sound_read(sound_p sound, void* buffer, size_t size) {
	snd_pcm_sframes_t frames_read = snd_pcm_readi(sound->pcm, buffer, snd_pcm_bytes_to_frames(sound->pcm, size));
	return (frames_read > 0) ? snd_pcm_frames_to_bytes(sound->pcm, frames_read) : frames_read;
}

ssize_t sound_write(sound_p sound, const void* buffer, size_t size) {
	snd_pcm_sframes_t frames_written = snd_pcm_writei(sound->pcm, buffer, snd_pcm_bytes_to_frames(sound->pcm, size));
	return (frames_written > 0) ? snd_pcm_frames_to_bytes(sound->pcm, frames_written) : frames_written;
}

ssize_t sound_recover(sound_p sound, ssize_t error) {
	return snd_pcm_recover(sound->pcm, error, true);
}


int signals_init() {
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

int signals_cleanup(int signal_fd) {
	close(signal_fd);
	
	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGINT);
	sigaddset(&signal_mask, SIGTERM);
	if ( sigprocmask(SIG_UNBLOCK, &signal_mask, NULL) == -1 )
		return perror("sigprocmask"), -1;
	
	return 1;
}


int main(int argc, char** argv) {
	argc = argc;
	size_t latency = 5;
	
	sound_p in = sound_open(argv[1], SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	sound_configure(in, 48000, 2, SND_PCM_FORMAT_S16, true, latency);
	
	sound_p out = sound_open("default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	sound_configure(out, 48000, 2, SND_PCM_FORMAT_S16, true, latency);
	
	
	size_t buffer_size = in->buffer_size;
	size_t buffer_filled = 0;
	void* buffer = malloc(in->buffer_size);
	
	int signals = signals_init();
	
	
	size_t poll_fd_count = sound_poll_fds_count(in) + sound_poll_fds_count(out);
	struct pollfd pollfds[poll_fd_count];
	
	timeval_t mark;
	size_t overruns = 0, underruns = 0, flushes = 0;
	while(true) {
		// Poll all FDs
		size_t poll_fds_used = 0;
		
		pollfds[0] = (struct pollfd){ .fd = signals, .events = POLLIN, .revents = 0 };
		poll_fds_used++;
		
		struct pollfd* in_poll_fds = pollfds + poll_fds_used;
		size_t in_poll_fd_count = sound_poll_fds(in,  pollfds + poll_fds_used, poll_fd_count - poll_fds_used);
		poll_fds_used += in_poll_fd_count;
		
		struct pollfd* out_poll_fds = pollfds + poll_fds_used;
		size_t out_poll_fd_count = sound_poll_fds(out, pollfds + poll_fds_used, poll_fd_count - poll_fds_used);
		poll_fds_used += out_poll_fd_count;
		
		mark = time_now();
		int active_fds = poll(pollfds, poll_fds_used, -1);
		if (active_fds == -1)
			return perror("poll"), 1;
		
		double poll_time = time_mark(&mark);
		printf("polling %zu fds, %d active, %4.1lf ms   ", poll_fds_used, active_fds, poll_time);
		
		// Check for incomming signals to shutdown the server
		if (pollfds[0].revents & POLLIN) {
			// Consume signal (so SIGTERM will not kill us after unblocking signals)
			struct signalfd_siginfo infos;
			if ( read(signals, &infos, sizeof(infos)) == -1 )
				return perror("read from signalfd"), 1;
			
			// Break poll loop
			printf("signal caught, exiting\n");
			break;
		}
		
		// Read and write pending audio data
		double read_time = 0, write_time = 0;
		ssize_t bytes_read = 0, bytes_written = 0;
		uint16_t in_revents  = sound_poll_fds_revents(in,  in_poll_fds,  in_poll_fd_count);
		uint16_t out_revents = sound_poll_fds_revents(out, out_poll_fds, out_poll_fd_count);
		
		if (in_revents & POLLIN) {
			mark = time_now();
			while( (bytes_read = sound_read(in, buffer + buffer_filled, buffer_size - buffer_filled)) < 0 ) {
				sound_recover(in, bytes_read);
				overruns++;
			}
			read_time = time_mark(&mark);
			buffer_filled += bytes_read;
		}
		
		if (out_revents & POLLOUT) {
			mark = time_now();
			while( (bytes_written = sound_write(out, buffer, buffer_filled)) < 0 ) {
				sound_recover(out, bytes_written);
				underruns++;
			}
			write_time = time_mark(&mark);
			if (bytes_written < (ssize_t)buffer_filled) {
				// Samples still left in buffer, move to front of buffer.
				memmove(buffer, buffer + bytes_written, buffer_filled - bytes_written);
				buffer_filled = buffer_filled - bytes_written;
			} else {
				buffer_filled = 0;
			}
		}
		
		if (buffer_filled > buffer_size / 2) {
			buffer_filled = 0;
			flushes++;
		}
		
		printf("%5zd bytes read, %4.1lf ms, %5zd bytes written, %4.1lf ms, %5zu buffer bytes, %4zu overruns, %4zu underruns, %4zu flushes\r",
			bytes_read, read_time, bytes_written, write_time, buffer_filled, overruns, underruns, flushes);
		fflush(stdout);
		
		/*
		
		uint16_t revents = sound_poll_fds_revents(sound, pollfds, dfilled);
		double revents_time = time_mark(&mark);
		if ( !(revents & POLLIN) ) {
			printf("poll: %4.1lf ms, revents: %4.1lf\n", poll_time, revents_time);
			continue;
		}
		
		bytes_read = sound_read(sound, buffer_ptr, sound->buffer_size);
		double readi_time = time_mark(&mark);
		if (bytes_read < 0) {
			printf("\n");
			sound_recover(sound, bytes_read);
			continue;
		}
		
		ssize_t bytes_written = sound_write(out, buffer_ptr, bytes_read);
		double write_time = time_mark(&mark);
		if (bytes_written < 0) {
			printf("\n");
			sound_recover(out, bytes_written);
		}
		
		
		
		int pcmreturn;
		while ((pcmreturn = sound_write(out, buffer_ptr, bytes_read)) < 0) {
			snd_pcm_prepare(out->pcm);
			fprintf(stderr, "\n<<<<<<<<<<<<<<< Buffer Underrun >>>>>>>>>>>>>>>\n");
		}
		double write_time = time_mark(&mark);
		
		printf("\r%zd bytes read, %zd bytes written, poll: %4.1lf ms, readi: %4.1lf ms, revents: %4.1lf, writei: %4.1f ms",
			bytes_read, bytes_written, poll_time, readi_time, revents_time, write_time);
		fflush(stdout);
		*/
	}
    
    //snd_pcm_drain(sound->pcm);
    
    sound_close(in);
    sound_close(out);
    signals_cleanup(signals);
}