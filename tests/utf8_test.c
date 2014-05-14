#include "testing.h"
#include "../utf8.h"


void test_zero_terminated_iteration() {
	struct { char* utf8; uint32_t* utf32; } test_cases[] = {
		// Empty string
		{ "", (uint32_t[]){ 0 } },
		
		// ASCII string
		{
			              "Hello World!\n",
			(uint32_t[]){ 'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '!', '\n', 0 }
		},
		
		// One UTF-8 code point
		{ "hö", (uint32_t[]){ 'h', 0x00F6, 0 } },
		
		// Lots of em
		{
			              "öäüßÖÄÜ€",
			(uint32_t[]){ 0x00F6, 0x00E4, 0x00FC, 0x00DF, 0x00D6, 0x00C4, 0x00DC, 0x20AC, 0 }
		},
		
		// End of string during code point, string is an 'ö' and an incomplete 'ö'
		{
			(char[])    {   0xC3, 0xB6,   0xC3, 0 },
			(uint32_t[]){       0x00F6, 0xFFFD, 0 }
		},
		
		// Incomplete code point (first byte of an 'ö') and a new code point right
		// behind it (a complete 'ö')
		{
			(char[])    {   0xC3, 0xC3, 0xB6, 0 },
			(uint32_t[]){ 0xFFFD,     0x00F6, 0 }
		},
		
		// Incomplete 3 byte code point (first byte of '€') and a new code point
		// right behind it (complete '€'). Makes sure the iteration doesn't skip
		// parts of the next code point.
		{
			(char[])    {   0xE2, 0xE2, 0x82, 0xAC, 0 },
			(uint32_t[]){ 0xFFFD,           0x20AC, 0 }
		},
		
		// Start iteration at an intermediate byte (second byte of 'ö''), then followed
		// by a complete code point ('ö').
		{
			(char[])    {   0xB6, 0xC3, 0xB6, 0 },
			(uint32_t[]){ 0xFFFD,     0x00F6, 0 }
		},
		
		// Start iteration at an intermediate byte (second byte of 'ö'') directly followed
		// by end of string.
		{
			(char[])    {   0xB6, 0 },
			(uint32_t[]){ 0xFFFD, 0 }
		},
		
		// Start of incomplete code point at end of string. Test if the iteration looks behind
		// the null terminator.
		{
			(char[])    { 0xE2, 0x82, 0xAC,   0xE2, 0, 1, 1, 1, 1, 1, 1 },
			(uint32_t[]){           0x20AC, 0xFFFD, 0 }
		},
		
		// Test all sizes of the encoding.
		{
			(char[])    { 0b01111111,  0b11011111, 0b10111111,  0b11101111, 0b10111111, 0b10111111, 0b11110111, 0b10111111, 0b10111111, 0b10111111, 0b11111011, 0b10111111, 0b10111111, 0b10111111, 0b10111111, 0b11111101, 0b10111111, 0b10111111, 0b10111111, 0b10111111, 0b10111111, 0 },
			(uint32_t[]){ 0b01111111,      0b0000011111111111,          0b000000001111111111111111,             0b00000000000111111111111111111111,                         0b00000011111111111111111111111111,                                     0b01111111111111111111111111111111, 0 }
		}
	};
	
	size_t test_case_count = sizeof(test_cases) /  sizeof(test_cases[0]);
	for (size_t i = 0; i < test_case_count; i++) {
		char*     utf8  = test_cases[i].utf8;
		uint32_t* utf32 = test_cases[i].utf32;
		
		for(utf8_iterator_t it = utf8_first(utf8); it.code_point != 0; it = utf8_next(it)) {
			uint32_t expected_code_point = *(utf32++);
			check_msg(it.code_point == expected_code_point, "got 0x%04X, expected 0x%04X, string \"%s\", code point index %zu",
				it.code_point, expected_code_point, utf8, utf32 - test_cases[i].utf32);
		}
		
		// Make sure the iteration carries us all the way to the end of the UTF-32 string
		check_msg(*utf32 ==  0, "not at end of string UTF-32 string, string \"%s\", got only %zu code points",
			utf8, utf32 - test_cases[i].utf32);
	}
}

void test_sized_iteration() {
	struct { char* utf8; size_t size; uint32_t* utf32; } test_cases[] = {
		// Empty string
		{
			              "", 0,
			(uint32_t[]){ 0 }
		},
		
		// Filled buffer but 0 size
		{
			              "Hello World!\n", 0,
			(uint32_t[]){ 0 }
		},
		
		// ASCII string
		{
			              "Hello World!\n", 1,
			(uint32_t[]){ 'H', 0 }
		},
		
		// Lots of em
		{
			              "öäüßÖÄÜ€", 4,
			(uint32_t[]){ 0x00F6, 0x00E4, 0 }
		},
		
		// End of string during code point, string is an 'ö' and an incomplete 'ö'
		{
			(char[])    {   0xC3, 0xB6,   0xC3, 0xB6, 0xC3, 0xB6, 0 }, 3,
			(uint32_t[]){       0x00F6, 0xFFFD,    0 }
		},
		
		// Start of incomplete code point at end of string. Test if the iteration looks behind
		// the null terminator.
		{
			(char[])    { 0xE2, 0x82, 0xAC,   0xE2, 0x82, 0xAC, 0 }, 4,
			(uint32_t[]){           0x20AC, 0xFFFD, 0 }
		},
		
		// Just a buffer full of intermediate bytes. Checks if the iteration stops at the end of
		// the buffer when seeking for a new code point start and we haven't got a zero terminator.
		// In the error case the iteration seeks over the end of the array and goes haywire.
		{
			(char[])    { 0x82, 0xAC, 0xB6, 0xB6, 0xB6 }, 3,
			(uint32_t[]){           0xFFFD, 0 }
		}
	};
	
	size_t test_case_count = sizeof(test_cases) /  sizeof(test_cases[0]);
	for (size_t i = 0; i < test_case_count; i++) {
		char*     utf8  = test_cases[i].utf8;
		size_t    size  = test_cases[i].size;
		uint32_t* utf32 = test_cases[i].utf32;
		
		for(utf8_iterator_t it = utf8_first_size(utf8, size); it.code_point != 0; it = utf8_next(it)) {
			uint32_t expected_code_point = *(utf32++);
			check_msg(it.code_point == expected_code_point, "got 0x%04X, expected 0x%04X, string \"%s\", code point index %zu",
				it.code_point, expected_code_point, utf8, utf32 - test_cases[i].utf32);
		}
		
		// Make sure the iteration carries us all the way to the end of the UTF-32 string
		check_msg(*utf32 ==  0, "not at end of string UTF-32 string, string \"%s\", got only %zu code points",
			utf8, utf32 - test_cases[i].utf32);
	}
}


int main(){
	run(test_zero_terminated_iteration);
	run(test_sized_iteration);
	
	return show_report();
}