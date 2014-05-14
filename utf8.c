#include <stdio.h>
#include "utf8.h"


utf8_iterator_t utf8_first_size(char* buffer, size_t size) {
	return utf8_next((utf8_iterator_t){
		.buffer = buffer,
		.end    = buffer + size,
		.code_point = 0
	});
}

utf8_iterator_t utf8_first(char* buffer) {
	// Use the highest possible memory addess as end (more or less UINTPTR_MAX)
	// so the size checks don't hit.
	return utf8_next((utf8_iterator_t){
		.buffer = buffer,
		.end    = (char*)UINTPTR_MAX,
		.code_point = 0
	});
}

utf8_iterator_t utf8_next(utf8_iterator_t it) {
	it.code_point = 0;
	// We're at the end of string, return 0 as code point, without dereferencing the buffer
	if (it.buffer == it.end)
		return it;
	
	uint8_t byte = *it.buffer;
	// We're at the zero terminator, return 0 as code point
	if (byte == 0)
		return it;
	it.buffer++;
	
	// __builtin_clz() counts the leading zeros but we want the leading ones. Therefore flip
	// all bits (~).
	// __builtin_clz() works on 32 bit ints, but we only want the leading one bits of our
	// 8 bit byte. Therefore put the byte at the highest order bits of the int (<< 24).
	int leading_ones = __builtin_clz(~byte << 24);
	
	if (leading_ones != 1) {
		// Store the data bits of the first byte in the code point
		int data_bits_in_first_byte = 8 - 1 - leading_ones;
		it.code_point = byte & ~(0xFFFFFFFF << data_bits_in_first_byte);
		
		ssize_t additional_bytes = leading_ones - 1;
		// additional_bytes is -1 when we have no further bytes for this code point (got a one byte
		// code point). This value is actually wrong (should be 0) but we don't need any special
		// handling for that case. The compare and loop both use signed compares so we're fine.
		// The for loop is completely skipped in that case, too.
		
		if (it.buffer + additional_bytes <= it.end) {
			for(ssize_t i = 0; i < additional_bytes; i++) {
				byte = *it.buffer;
				
				if ( (byte & 0xC0) == 0x80 ) {
					// Make room in it.code_point for 6 more bits and OR the current bytes data
					// bits in there.
					it.code_point <<= 6;
					it.code_point |= byte & 0x3F;
				} else {
					// Error, this isn't an itermediate byte! It's either the zero terminator or
					// the start of a new code point. In both cases we'll return the replacement
					// character to signal the current broken code point. Leave the buffer at the
					// current position so the next call sees either the new code point or the
					// zero terminator.
					it.code_point = 0xFFFD;
					break;
				}
				
				it.buffer++;
			}
		} else {
			// Error, buffer doesn't contain all the bytes of this code point. Return the replacement
			// character and set the buffer to the end.
			it.code_point = 0xFFFD;
			it.buffer = it.end;
		}
	} else {
		// Error, we're at an intermediate byte.
		// Skip all intermediate bytes (or to the end of the buffer) and return the replacement
		// character.
		while ( (*(it.buffer) & 0xC0) == 0x80 && it.buffer < it.end )
			it.buffer++;
		it.code_point = 0xFFFD;
	}
	
	return it;
}