#include <string.h>

#include <alloca.h>
#include <alsa/asoundlib.h>

#include "sound.h"


sound_p sound_open(const char* name, sound_stream_type_t type, int flags) {
	sound_p sound = malloc(sizeof(sound_t));
	memset(sound, 0, sizeof(sound_t));
	
	snd_pcm_open((snd_pcm_t**)&sound->pcm, name, type, flags);
	
	return sound;
}

void sound_close(sound_p sound) {
	snd_pcm_close(sound->pcm);
	memset(sound, 0, sizeof(sound_t));
	free(sound);
}

#define error_checked(func, message) if((func) < 0){ printf(message); return; }

void sound_configure(sound_p sound, uint32_t rate, uint32_t channels, sound_format_t format, bool interleaved, size_t period_time_ms) {
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

#undef error_checked

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