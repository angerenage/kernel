#include <base/math.h>
#include <criterion/criterion.h>
#include <stdint.h>

Test(base_math, overflow_helpers_detect_success_and_overflow) {
	uint64_t sum64        = 0u;
	uint64_t product64    = 0u;
	size_t   sum_size     = 0u;
	size_t   product_size = 0u;

	cr_assert_not(add_overflow_u64(4u, 5u, &sum64));
	cr_assert_eq(sum64, 9u);
	cr_assert(add_overflow_u64(UINT64_MAX, 1u, &sum64));

	cr_assert_not(mul_overflow_u64(7u, 8u, &product64));
	cr_assert_eq(product64, 56u);
	cr_assert(mul_overflow_u64(UINT64_MAX, 2u, &product64));

	cr_assert_not(add_overflow_size(3u, 4u, &sum_size));
	cr_assert_eq(sum_size, 7u);
	cr_assert(add_overflow_size((size_t)-1, 1u, &sum_size));

	cr_assert_not(mul_overflow_size(5u, 6u, &product_size));
	cr_assert_eq(product_size, 30u);
	cr_assert(mul_overflow_size((size_t)-1, 2u, &product_size));
}

Test(base_math, alignment_helpers_round_and_normalize) {
	uint64_t aligned_u64  = 0u;
	size_t   aligned_size = 0u;

	cr_assert(align_up_u64(17u, 16u, &aligned_u64));
	cr_assert_eq(aligned_u64, 32u);
	cr_assert_eq(align_down_u64(31u, 16u), 16u);
	cr_assert_not(align_up_u64(5u, 3u, &aligned_u64));

	cr_assert(align_up_size(33u, 32u, &aligned_size));
	cr_assert_eq(aligned_size, 64u);
	cr_assert_not(align_up_size(9u, 6u, &aligned_size));

	cr_assert_eq(normalize_align_u64(0u, 4096u), 4096u);
	cr_assert_eq(normalize_align_u64(64u, 4096u), 64u);
}
