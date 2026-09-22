#pragma once

/* Reserved vectors and legacy interrupt-vector range. */
#define X86_IRQ_BASE 32u
#define X86_IRQ_COUNT 16u
#define X86_SYSCALL_VECTOR 0x80u
#define X86_LAPIC_SPURIOUS_VECTOR 0xffu
#define X86_LAPIC_WAKE_VECTOR 0xfeu
