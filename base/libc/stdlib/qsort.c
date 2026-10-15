#include <stdint.h>
#include <stdlib.h>

static void swap(unsigned char* left, unsigned char* right, size_t size) {
	for (size_t index = 0u; index < size; index++) {
		unsigned char byte = left[index];
		left[index]        = right[index];
		right[index]       = byte;
	}
}

static void sift_down(unsigned char* bytes, size_t root, size_t count, size_t size,
                      int (*compare)(const void*, const void*)) {
	if (count < 2u) return;
	while (root <= (count - 2u) / 2u) {
		size_t child = root * 2u + 1u;
		if (child + 1u < count && compare(bytes + child * size, bytes + (child + 1u) * size) < 0) child++;
		if (compare(bytes + root * size, bytes + child * size) >= 0) return;
		swap(bytes + root * size, bytes + child * size, size);
		root = child;
	}
}

void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*)) {
	if (count < 2u || size == 0u) return;
	if (count > SIZE_MAX / size) return;
	unsigned char* bytes = base;
	for (size_t root = count / 2u; root != 0u;) {
		root--;
		sift_down(bytes, root, count, size, compare);
	}
	for (size_t end = count - 1u; end != 0u; end--) {
		swap(bytes, bytes + end * size, size);
		sift_down(bytes, 0u, end, size, compare);
	}
}
