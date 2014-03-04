/**

The constant values are taken from /usr/include/alsa/pcm.h to match
ALSAs constants.

*/
#pragma once

#include <stdint.h>
#include <stdbool.h>


typedef struct {
	void*      pcm;
	uint32_t   rate;
	uint32_t   channels;
	size_t     sample_size;
	size_t     buffer_size;
} sound_t, *sound_p;

typedef enum {
	SOUND_PLAYBACK = 0,
	SOUND_CAPTURE  = 1
} sound_stream_type_t;

#define SOUND_NONBLOCK 0x00000001

typedef enum {
	SOUND_FORMAT_S16 = 2
} sound_format_t;


sound_p  sound_open(const char* name, sound_stream_type_t type, int flags);
void     sound_configure(sound_p sound, uint32_t rate, uint32_t channels, sound_format_t format, bool interleaved, size_t period_time_ms);
void     sound_close(sound_p sound);

ssize_t  sound_read(sound_p sound, void* buffer, size_t size);
ssize_t  sound_write(sound_p sound, const void* buffer, size_t size);
ssize_t  sound_recover(sound_p sound, ssize_t error);

size_t   sound_poll_fds_count(sound_p sound);
size_t   sound_poll_fds(sound_p sound, struct pollfd* fds, size_t nfds);
uint16_t sound_poll_fds_revents(sound_p sound, struct pollfd* fds, size_t nfds);