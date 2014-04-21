CC     = gcc
CFLAGS = -std=c99 -Werror -Wall -Wextra -Wno-unused-parameter
# The Pulse Audio callbacks require a full signature but not all parameters
# are used all the time. Therefore disable the unused-parameter warning.

CFLAGS := $(CFLAGS) -g
#CFLAGS := $(CFLAGS) -Ofast -mtune=native -march=native


#
# Real applications, object files are created by implicit rules
#
hdswitch: LDLIBS = deps/libSDL2.a -ldl -lrt -lm `pkg-config --libs gl` -lasound
hdswitch: deps/libSDL2.a hdswitch.o drawable.o stb_image.o cam.o sound.o ebml_writer.o array.o

hdswitch.o: deps/libSDL2.a
hdswitch.o: CFLAGS := $(CFLAGS) -Ideps/include `pkg-config --cflags gl` -Wno-multichar -Wno-unused-but-set-variable -Wno-unused-variable

experiments/v4l2_cam: CFLAGS := $(CFLAGS) -Wno-multichar -Wno-unused-variable -Ideps/include `pkg-config --cflags gl`
experiments/v4l2_cam: LDLIBS = deps/libSDL2.a -ldl -lrt -lm `pkg-config --libs gl`
experiments/v4l2_cam: cam.o deps/libSDL2.a drawable.o

experiments/alsa: LDLIBS = -lasound
experiments/alsa_rec: LDLIBS = -lasound
experiments/alsa_pcm_list: LDLIBS = -lasound

experiments/fbo: CFLAGS := $(CFLAGS) -Ideps/include `pkg-config --cflags gl`
experiments/fbo: LDLIBS = deps/libSDL2.a -ldl -lrt -lm `pkg-config --libs gl`
experiments/fbo: deps/libSDL2.a drawable.o stb_image.o

experiments/pulse: CFLAGS := $(CFLAGS) -Wno-unused-parameter
experiments/pulse: LDLIBS  = -lpulse

#
# Special parameters for some objects files not really under our control
#
stb_image.o: CFLAGS := $(CFLAGS) -Wno-sign-compare -Wno-unused-but-set-variable
# Need GNU99 since C99 removes ftello() from stdin.h. Found no other way
# to make it work yet.
ebml_writer.o: CFLAGS := $(CFLAGS) -std=gnu99

#
# Download and build SDL2 as static library.
# Use the 2.0.2 development build right now because SDL_SetRelativeMouseMode() is
# broken in the 2.0.1 release.
#
deps/libSDL2.tar.gz:
	wget http://www.libsdl.org/tmp/SDL-2.0.2-8197.tar.gz -O deps/libSDL2.tar.gz

deps/libSDL2.a: deps/libSDL2.tar.gz
	cd deps;       tar -xaf libSDL2.tar.gz
	mv deps/SDL* deps/SDL2
	cd deps/SDL2;  ./configure --disable-shared
	cd deps/SDL2;  make -j
	mv deps/SDL2/build/.libs/libSDL2.a deps/
	mkdir -p deps/include
	mv deps/SDL2/include deps/include/SDL
	rm -rf deps/SDL2


# Clean all files listed in .gitignore, except sublime project files. Ensures the
# ignore list is properly maintained.
clean:
	rm -fr `grep -v sublime .gitignore | tr '\n' ' '`