#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pulse/pulseaudio.h>

bool server_start(const char* socket_path, uint16_t width, uint16_t height, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample, pa_mainloop_api* mainloop);
void server_stop();
void server_enqueue_frame(uint8_t track, uint64_t timecode_us, void* frame_data, size_t frame_size);
void server_flush_and_disconnect_clients();