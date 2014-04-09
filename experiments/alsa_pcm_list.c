#include <stdio.h>
#include <string.h>
#include <alsa/asoundlib.h>

int main(int argc, char** argv) {
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s Input|Output\n", argv[0]);
        fprintf(stderr, "The \"Input\" filter lists all mics, the \"Output\" filter all playback devices.\n");
        fprintf(stderr, "The filter name is case senstive!\n");
        return 1;
    }
    
    void **hints;
    char *name, *desc, *io;
    const char *filter = argv[1];
    
    int err = snd_device_name_hint(-1, "pcm", &hints);
    if (err != 0)
        return err;
    
    //filter = stream == SND_PCM_STREAM_CAPTURE ? "Input" : "Output";
    
    for(void** n = hints; *n != NULL; n++) {
        name = snd_device_name_get_hint(*n, "NAME");
        desc = snd_device_name_get_hint(*n, "DESC");
        io   = snd_device_name_get_hint(*n, "IOID");
        
        if ( io != NULL && strcmp(io, filter) == 0 ) {
            printf("name: %s\n", name);
            printf("  desc: %s\n", desc);
            printf("  io: %s\n", io);
        }
        
        free(name);
        free(desc);
        free(io);
    }
    
    snd_device_name_free_hint(hints);
    
    return 0;
}