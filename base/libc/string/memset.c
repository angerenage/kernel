#include <stdint.h>
#include <string.h>

typedef size_t memset_word_t __attribute__((__may_alias__));

void* memset(void* dest, int value, size_t count) {
	void* result = dest;

#if defined(__x86_64__)
	__asm__ volatile("rep stosb" : "+D"(dest), "+c"(count) : "a"((unsigned char)value) : "memory");
#else
	unsigned char* bytes     = dest;
	unsigned char  byte      = (unsigned char)value;
	const size_t   word_size = sizeof(memset_word_t);

	while (count != 0u && ((uintptr_t)bytes & (word_size - 1u)) != 0u) {
		*bytes++ = byte;
		count--;
	}
	memset_word_t word = (memset_word_t)byte * (~(memset_word_t)0u / 0xffu);
	while (count >= word_size * 4u) {
		memset_word_t* words = (memset_word_t*)bytes;
		words[0]             = word;
		words[1]             = word;
		words[2]             = word;
		words[3]             = word;
		bytes += word_size * 4u;
		count -= word_size * 4u;
	}
	while (count >= word_size) {
		*(memset_word_t*)bytes = word;
		bytes += word_size;
		count -= word_size;
	}
	while (count-- != 0u) *bytes++ = byte;
#endif

	return result;
}
