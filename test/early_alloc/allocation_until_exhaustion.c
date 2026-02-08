#include "early_alloc_test.h"

Test(early_alloc, allocation_until_exhaustion) {
	_Alignas(4096) uint8_t arena[KiB(1024)];
	init_early_allocator(arena, sizeof(arena));

	const size_t block = KiB(64);
	size_t       count = 0;

	while (1) {
		uint64_t r0 = early_remaining_bytes();
		void*    p  = early_alloc(block, 16);
		uint64_t r1 = early_remaining_bytes();

		if (!p) {
			cr_assert(r0 <= sizeof(arena), "early_remaining_bytes out of expected range");
			break;
		}

		uint8_t* b   = (uint8_t*)p;
		b[0]         = 0x5Au;
		b[block - 1] = 0xA5u;

		count++;
		cr_assert(r1 <= r0, "early_remaining_bytes increased during stress test");
	}

	cr_assert(count > 0, "stress test did not allocate any blocks");
}
