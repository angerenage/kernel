#include <stdint.h>
#include <string.h>

typedef size_t memcpy_word_t __attribute__((__may_alias__));

void* memcpy(void* restrict dest, const void* restrict src, size_t count) {
#if defined(__x86_64__)
	void* result = dest;
	__asm__ volatile("rep movsb" : "+D"(dest), "+S"(src), "+c"(count) : : "memory");
	return result;
#else
	unsigned char*       dest_bytes = dest;
	const unsigned char* src_bytes  = src;
	const size_t         word_size  = sizeof(memcpy_word_t);

	if ((((uintptr_t)dest_bytes ^ (uintptr_t)src_bytes) & (word_size - 1u)) == 0u) {
		while (count != 0u && ((uintptr_t)dest_bytes & (word_size - 1u)) != 0u) {
			*dest_bytes++ = *src_bytes++;
			count--;
		}
		memcpy_word_t*       dest_words = (memcpy_word_t*)dest_bytes;
		const memcpy_word_t* src_words  = (const memcpy_word_t*)src_bytes;
		while (count >= word_size * 4u) {
			dest_words[0] = src_words[0];
			dest_words[1] = src_words[1];
			dest_words[2] = src_words[2];
			dest_words[3] = src_words[3];
			dest_words += 4u;
			src_words += 4u;
			count -= word_size * 4u;
		}
		while (count >= word_size) {
			*dest_words++ = *src_words++;
			count -= word_size;
		}
		dest_bytes = (unsigned char*)dest_words;
		src_bytes  = (const unsigned char*)src_words;
	}
	while (count-- != 0u) *dest_bytes++ = *src_bytes++;

	return dest;
#endif
}
