#include <stdio.h>
#include <glob.h>
#include "../cam.h"

int main() {
	glob_t files;
	int result = glob("/dev/video*", 0, NULL, &files);
	if (result == GLOB_NOMATCH) {
		fprintf(stderr, "no cams found!\n");
		return 0;
	} else if (result != 0) {
		return 1;
	}
	
	for(size_t i = 0; i < files.gl_pathc; i++) {
		printf("%s\n", files.gl_pathv[i]);
		cam_p cam = cam_open(files.gl_pathv[i]);
		cam_print_info(cam);
		cam_close(cam);
	}
	
	globfree(&files);
}