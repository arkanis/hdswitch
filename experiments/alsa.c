#include <stdint.h>
#include <alloca.h>
#include <alsa/asoundlib.h>

#define error_checked(func, message) if((func) < 0){ return printf(message), 1; }

int main() {
	snd_pcm_t* sound = NULL;
	snd_pcm_hw_params_t* hwparams = NULL;
	
	snd_pcm_hw_params_alloca(&hwparams);
	error_checked( snd_pcm_open(&sound, "default", SND_PCM_STREAM_PLAYBACK, 0), "Error opening PCM device\n" );
	error_checked( snd_pcm_hw_params_any(sound, hwparams), "Can not configure this PCM device\n" );
	
	error_checked( snd_pcm_hw_params_set_access(sound, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED), "Error setting access\n");
	error_checked( snd_pcm_hw_params_set_format(sound, hwparams, SND_PCM_FORMAT_S16_LE), "Error setting format\n");
	uint32_t rate = 48000;
	error_checked( snd_pcm_hw_params_set_rate_near(sound, hwparams, &rate, 0), "Error setting rate.\n");
	printf("rate: %d\n", rate);
	
	int channels = 2;
	error_checked( snd_pcm_hw_params_set_channels(sound, hwparams, channels), "Error setting channels.\n");
	uint32_t periods = 2;
	error_checked( snd_pcm_hw_params_set_periods_near(sound, hwparams, &periods, NULL), "Error setting periods.\n");
	printf("periods: %d\n", periods);
	
	int bytes_per_frame = channels * sizeof(uint16_t);
	int periodsize = 8192;
	float latency = (periodsize * periods) / (float)(rate * bytes_per_frame);
	error_checked( snd_pcm_hw_params_set_buffer_size(sound, hwparams, (periodsize * periods) / bytes_per_frame), "Error setting buffersize.\n" );
	printf("latency: %f\n", latency);
	
	error_checked( snd_pcm_hw_params(sound, hwparams), "Error setting HW params.\n" );
	
	
    unsigned char *data;
    int pcmreturn, l1, l2;
    short s1, s2;
    int frames;
    int num_frames = periodsize / bytes_per_frame;

    data = (unsigned char *)malloc(periodsize);
    frames = periodsize >> 2;
    for(l1 = 0; l1 < 100; l1++) {
      for(l2 = 0; l2 < num_frames; l2++) {
        s1 = (l2 % 128) * 100 - 5000;  
        s2 = (l2 % 256) * 100 - 5000;  
        data[4*l2] = (unsigned char)s1;
        data[4*l2+1] = s1 >> 8;
        data[4*l2+2] = (unsigned char)s2;
        data[4*l2+3] = s2 >> 8;
      }
      while ((pcmreturn = snd_pcm_writei(sound, data, frames)) < 0) {
        snd_pcm_prepare(sound);
        fprintf(stderr, "<<<<<<<<<<<<<<< Buffer Underrun >>>>>>>>>>>>>>>\n");
      }
    }
    
    snd_pcm_drain(sound);
    snd_pcm_close(sound);
}