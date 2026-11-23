#include <stdint.h>

#include <criterion/criterion.h>

#include "../../../platforms/pc_x86_64/hal/paging_policy.h"

Test(x86_64_paging_policy, accepts_write_back_at_normal_pat_index) {
	uint64_t pat = UINT64_C(0x0000000000000006);

	cr_assert(x86_pat_normal_memory_supported(true, pat));
	cr_assert_eq(x86_pat_entry(pat, 0u), X86_PAT_WB);
}

Test(x86_64_paging_policy, rejects_non_write_back_normal_pat_index) {
	cr_assert_not(x86_pat_normal_memory_supported(true, UINT64_C(0x0000000000000000)));
	cr_assert_not(x86_pat_normal_memory_supported(true, UINT64_C(0x0000000000000001)));
}

Test(x86_64_paging_policy, permits_legacy_normal_encoding_without_pat) {
	cr_assert(x86_pat_normal_memory_supported(false, UINT64_MAX));
}
