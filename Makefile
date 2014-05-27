CC     = gcc
CFLAGS = -std=c99 -Werror -Wall -Wextra -Wno-unused-parameter
# The Pulse Audio callbacks require a full signature but not all parameters
# are used all the time. Therefore disable the unused-parameter warning.

CFLAGS := $(CFLAGS) -g
#CFLAGS := $(CFLAGS) -Ofast -mtune=native -march=native


#
# Real applications, object files are created by implicit rules
#
hdswitch: LDLIBS = deps/libSDL2.a -pthread -ldl -lrt -lm `pkg-config --libs gl libpulse freetype2`
hdswitch: deps/libSDL2.a hdswitch.o server.o mixer.o drawable.o stb_image.o cam.o ebml_writer.o array.o hash.o utf8.o list.o text_renderer.o

hdswitch.o: deps/libSDL2.a
hdswitch.o: CFLAGS := $(CFLAGS) -Ideps/include `pkg-config --cflags gl libpulse freetype2` -Wno-multichar -Wno-unused-but-set-variable -Wno-unused-variable

text_renderer.o: CFLAGS := $(CFLAGS) `pkg-config --cflags freetype2`

experiments/v4l2_cam: CFLAGS := $(CFLAGS) -Wno-multichar -Wno-unused-variable -Ideps/include `pkg-config --cflags gl`
experiments/v4l2_cam: LDLIBS = deps/libSDL2.a -ldl -lrt -lm `pkg-config --libs gl`
experiments/v4l2_cam: cam.o deps/libSDL2.a drawable.o

experiments/v4l2_list: cam.o

experiments/alsa: LDLIBS = -lasound
experiments/alsa_rec: LDLIBS = -lasound
experiments/alsa_pcm_list: LDLIBS = -lasound

experiments/fbo: CFLAGS := $(CFLAGS) -Ideps/include `pkg-config --cflags gl`
experiments/fbo: LDLIBS = deps/libSDL2.a -ldl -lrt -lm `pkg-config --libs gl`
experiments/fbo: deps/libSDL2.a drawable.o stb_image.o

experiments/pulse: CFLAGS := $(CFLAGS) -Wno-unused-parameter
experiments/pulse: LDLIBS  = -lpulse

experiments/text_rendering: CFLAGS := $(CFLAGS) -Ideps/include `pkg-config --cflags gl freetype2`
experiments/text_rendering: LDLIBS = deps/libSDL2.a -ldl -lrt -lm `pkg-config --libs gl freetype2`
experiments/text_rendering: stb_image.o drawable.o text_renderer.o hash.o array.o utf8.o

experiments/gui: CFLAGS := $(CFLAGS) -Ideps/include `pkg-config --cflags gl freetype2`
experiments/gui: LDLIBS = deps/libSDL2.a -ldl -lrt -lm `pkg-config --libs gl freetype2`
experiments/gui: stb_image.o drawable.o text_renderer.o hash.o array.o utf8.o tree.o

tests/utf8_test: utf8.o tests/testing.o


#
# Special parameters for some objects files not really under our control
#
stb_image.o: CFLAGS := $(CFLAGS) -Wno-sign-compare -Wno-unused-but-set-variable
# Need GNU99 since C99 removes ftello() from stdin.h. Found no other way
# to make it work yet.
ebml_writer.o: CFLAGS := $(CFLAGS) -std=gnu99

#
# Download and build SDL2 as static library.
#
deps/libSDL2.tar.gz:
	wget http://libsdl.org/release/SDL2-2.0.3.tar.gz -O deps/libSDL2.tar.gz

deps/libSDL2.a: deps/libSDL2.tar.gz
	cd deps;       tar -xaf libSDL2.tar.gz
	cd deps;       rm -rf SDL2
	mv deps/SDL2-* deps/SDL2
	cd deps/SDL2;  ./configure --disable-shared
	cd deps/SDL2;  make -j
	mv deps/SDL2/build/.libs/libSDL2.a deps/
	mkdir -p deps/include
	mv deps/SDL2/include deps/include/SDL
	rm -rf deps/SDL2

deps/ubuntu:
	sudo apt-get install build-essential libgl1-mesa-dev libpulse-dev

# Clean all files listed in .gitignore, except sublime project files. Ensures the
# ignore list is properly maintained.
clean:
	rm -fr `grep -v sublime .gitignore | tr '\n' ' '`

# Make the project as clean as after a git clone.
veryclean: clean
	rm -rf deps/*
