#pragma once

#include <stdint.h>
#include <unistd.h>


typedef struct {
	char*    buffer;
	char*    end;
	uint32_t code_point;
} utf8_iterator_t;


utf8_iterator_t utf8_first_size(char* buffer, size_t size);
utf8_iterator_t utf8_first(char* buffer);
utf8_iterator_t utf8_next(utf8_iterator_t it);