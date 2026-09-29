#pragma once

#include <stdbool.h>

/* Complete a pending TLB shootdown request on the current CPU. */
bool loongarch64_paging_handle_tlb_ipi(void);
