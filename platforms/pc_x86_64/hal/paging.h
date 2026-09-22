#pragma once

#include <stdbool.h>

/* Handle a TLB-shootdown NMI on the local CPU. */
bool x86_64_paging_handle_tlb_nmi(void);
