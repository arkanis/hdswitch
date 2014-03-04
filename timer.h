#pragma once

#include <sys/time.h>
#include <stddef.h>
typedef struct timeval timeval_t, *timeval_p;


static inline timeval_t time_now(){
	timeval_t now;
	gettimeofday(&now, NULL);
	return now;
}

/**
 * Returns the time elapsed since `start` in milliseconds.
 */
static inline double time_since(timeval_t start){
	timeval_t now;
	gettimeofday(&now, NULL);
	return ( (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1000000.0 ) * 1000;
}

/**
 * Returns the time elapsed since `mark` in milliseconds and sets `mark` to the current time.
 * Meant to be used to measure continuous operations, e.g. time to calculate frames.
 */
static inline double time_mark(timeval_p mark){
	timeval_t now;
	gettimeofday(&now, NULL);
	double elapsed = (now.tv_sec - mark->tv_sec) + (now.tv_usec - mark->tv_usec) / 1000000.0;
	*mark = now;
	return elapsed * 1000;
}