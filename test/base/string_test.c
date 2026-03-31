#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>

Test(base_string, strlen_counts_until_nul) {
	cr_assert_eq(strlen(""), 0);
	cr_assert_eq(strlen("kernel"), 6);
	cr_assert_eq(strlen("with space"), 10);
}

Test(base_string, memset_fills_requested_range) {
	uint8_t bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};

	void* result = memset(bytes + 2, 0xaa, 4);

	cr_assert_eq(result, bytes + 2);
	cr_assert_eq(bytes[0], 0);
	cr_assert_eq(bytes[1], 1);
	cr_assert_eq(bytes[2], 0xaa);
	cr_assert_eq(bytes[3], 0xaa);
	cr_assert_eq(bytes[4], 0xaa);
	cr_assert_eq(bytes[5], 0xaa);
	cr_assert_eq(bytes[6], 6);
	cr_assert_eq(bytes[7], 7);
}

Test(base_string, memcpy_copies_forward_without_touching_neighbors) {
	uint8_t src[6]  = {10, 20, 30, 40, 50, 60};
	uint8_t dest[8] = {1, 2, 3, 4, 5, 6, 7, 8};

	void* result = memcpy(dest + 1, src + 1, 4);

	cr_assert_eq(result, dest + 1);
	cr_assert_eq(dest[0], 1);
	cr_assert_eq(dest[1], 20);
	cr_assert_eq(dest[2], 30);
	cr_assert_eq(dest[3], 40);
	cr_assert_eq(dest[4], 50);
	cr_assert_eq(dest[5], 6);
	cr_assert_eq(dest[6], 7);
	cr_assert_eq(dest[7], 8);
}

Test(base_string, memmove_handles_overlap_when_destination_is_higher) {
	char buffer[] = "abcdef";

	void* result = memmove(buffer + 2, buffer, 4);

	cr_assert_eq(result, buffer + 2);
	cr_assert_str_eq(buffer, "ababcd");
}

Test(base_string, memmove_handles_overlap_when_source_is_higher) {
	char buffer[] = "abcdef";

	void* result = memmove(buffer, buffer + 2, 4);

	cr_assert_eq(result, buffer);
	cr_assert(buffer[0] == 'c');
	cr_assert(buffer[1] == 'd');
	cr_assert(buffer[2] == 'e');
	cr_assert(buffer[3] == 'f');
	cr_assert(buffer[4] == 'e');
	cr_assert(buffer[5] == 'f');
}

Test(base_string, memcmp_reports_equality_and_order) {
	const uint8_t lhs[]  = {1, 2, 3, 4};
	const uint8_t rhs[]  = {1, 2, 3, 4};
	const uint8_t low[]  = {1, 2, 3, 3};
	const uint8_t high[] = {1, 2, 3, 5};

	cr_assert_eq(memcmp(lhs, rhs, sizeof(lhs)), 0);
	cr_assert(memcmp(lhs, low, sizeof(lhs)) > 0);
	cr_assert(memcmp(lhs, high, sizeof(lhs)) < 0);
}
