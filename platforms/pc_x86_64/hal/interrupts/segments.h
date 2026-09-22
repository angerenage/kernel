#pragma once

/* Global descriptor table selectors shared by entry and return paths. */
#define X86_GDT_KERNEL_CODE_SELECTOR 0x08u
#define X86_GDT_KERNEL_DATA_SELECTOR 0x10u
#define X86_GDT_USER_COMPAT_SELECTOR 0x18u
#define X86_GDT_USER_DATA_SELECTOR 0x20u
#define X86_GDT_USER_CODE_SELECTOR 0x28u
#define X86_GDT_TSS_SELECTOR 0x30u
