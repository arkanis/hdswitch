#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pulse/pulseaudio.h>
#include "timer.h"

bool mixer_start(usec_t start_walltime, uint32_t requested_latency_ms, uint32_t buffer_time_ms, uint32_t max_latency_to_block_for_ms, pa_sample_spec sample_spec, pa_mainloop_api* mainloop);
void mixer_stop();