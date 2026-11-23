#include <criterion/criterion.h>

#include "../../../platforms/pc_riscv64/hal/paging_policy.h"

Test(riscv64_paging_policy, advertises_only_exact_memory_type_encodings) {
	cr_assert(riscv_paging_memory_type_supported(MEMORY_TYPE_NORMAL, false));
	cr_assert_not(riscv_paging_memory_type_supported(MEMORY_TYPE_DEVICE, false));
	cr_assert_not(riscv_paging_memory_type_supported(MEMORY_TYPE_WRITE_COMBINING, false));

	cr_assert(riscv_paging_memory_type_supported(MEMORY_TYPE_NORMAL, true));
	cr_assert(riscv_paging_memory_type_supported(MEMORY_TYPE_DEVICE, true));
	cr_assert(riscv_paging_memory_type_supported(MEMORY_TYPE_WRITE_COMBINING, true));
	cr_assert_not(riscv_paging_memory_type_supported(MEMORY_TYPE_COUNT, true));
}
