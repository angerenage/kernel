#include <core/cpu.h>
#include <core/exception.h>
#include <core/sched.h>
#include <core/vm_space.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/hcf.h>
#include <hal/userspace.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../../platforms/pc_aarch64/hal/interrupts_private.h"

#define AARCH64_TEST_PSTATE_F (1ull << 6)
#define AARCH64_TEST_PSTATE_I (1ull << 7)

char exception_vectors[2048];

static size_t                observed_vmm_faults;
static uintptr_t             observed_vmm_addr;
static enum vmm_fault_kind   observed_fault_kind;
static enum vmm_fault_access observed_vmm_access;
static bool                  observed_vmm_user_mode;
static bool                  vmm_fault_result;

static size_t                   observed_core_faults;
static enum core_exception_kind observed_core_kind;

bool clock_handle_irq(const struct exception_frame* frame) {
	(void)frame;
	return false;
}

bool aarch64_handle_syscall(struct exception_frame* frame, uint64_t ec) {
	(void)frame;
	(void)ec;
	return false;
}

bool sched_handle_interrupt_exit(void) {
	return false;
}

void sched_finish_context_switch(void) {
}

void cpu_interrupts_set_ready(struct cpu* cpu, bool ready) {
	(void)cpu;
	(void)ready;
}

void cpu_enter_exception(void) {
}

void cpu_leave_exception(void) {
}

bool vm_handle_current_page_fault(uintptr_t addr, enum vmm_fault_kind kind, enum vmm_fault_access access,
                                  bool user_mode) {
	observed_vmm_faults++;
	observed_vmm_addr      = addr;
	observed_fault_kind    = kind;
	observed_vmm_access    = access;
	observed_vmm_user_mode = user_mode;
	return vmm_fault_result;
}

bool core_handle_user_exception(enum core_exception_kind kind) {
	observed_core_faults++;
	observed_core_kind = kind;
	return true;
}

bool core_handle_exception(enum core_exception_kind kind, enum core_exception_access access, uintptr_t addr,
                           bool user_mode) {
	(void)access;
	(void)addr;
	(void)user_mode;
	observed_core_faults++;
	observed_core_kind = kind;
	return true;
}

__attribute__((noreturn))
void hcf(void) {
	abort();
}

void aarch64_userspace_enter(void) {
}

static struct hal_cpu_fp_context live_fp_context;

void hal_cpu_fp_context_init(struct hal_cpu_fp_context* context) {
	if (context != NULL) memset(context, 0, sizeof(*context));
}

void hal_cpu_fp_context_save(struct hal_cpu_fp_context* context) {
	if (context != NULL) *context = live_fp_context;
}

void hal_cpu_fp_context_restore(const struct hal_cpu_fp_context* context) {
	if (context != NULL) live_fp_context = *context;
}

/*
 * Include the platform implementations directly so this native-architecture
 * test can exercise their internal exception classification without adding
 * test-only exports to the HAL.
 */
#include "../../../platforms/pc_aarch64/hal/interrupts.c"
#include "../../../platforms/pc_aarch64/hal/userspace.c"

static void aarch64_exception_test_reset(void) {
	observed_vmm_faults    = 0u;
	observed_vmm_addr      = 0u;
	observed_fault_kind    = VMM_FAULT_INVALID;
	observed_vmm_access    = VMM_FAULT_ACCESS_UNKNOWN;
	observed_vmm_user_mode = false;
	vmm_fault_result       = true;
	observed_core_faults   = 0u;
	observed_core_kind     = CORE_EXCEPTION_UNKNOWN;
	memset(&live_fp_context, 0, sizeof(live_fp_context));
}

Test(aarch64_exception_return, initial_el0_context_leaves_irq_and_fiq_unmasked) {
	struct thread_context context;
	uint64_t              spsr;

	aarch64_exception_test_reset();
	memset(&context, 0, sizeof(context));

	cr_assert(hal_userspace_thread_context_init(&context, 0x10000u, 0x4000u, 0x8000u, 0x11u));
	spsr = context.spill[AARCH64_THREAD_CTX_X22];

	cr_assert_eq(spsr & AARCH64_SPSR_MODE_MASK, AARCH64_SPSR_MODE_EL0T);
	cr_assert_eq(
		spsr & AARCH64_TEST_PSTATE_I, 0u, "new userspace threads must not enter EL0 with IRQ exceptions masked");
	cr_assert_eq(
		spsr & AARCH64_TEST_PSTATE_F, 0u, "new userspace threads must not enter EL0 with FIQ exceptions masked");
}

Test(aarch64_exception_return, translation_faults_still_reach_lazy_page_fault_policy) {
	struct exception_frame frame = {
		.vector = 8u,
		.esr    = (0x24ull << 26) | 0x04u,
		.far    = 0x12345000u,
	};

	aarch64_exception_test_reset();
	handle_exception(&frame);

	cr_assert_eq(observed_vmm_faults, 1u);
	cr_assert_eq(observed_vmm_addr, frame.far);
	cr_assert_eq(observed_fault_kind, VMM_FAULT_NOT_PRESENT);
	cr_assert_eq(observed_vmm_access, VMM_FAULT_ACCESS_READ);
	cr_assert(observed_vmm_user_mode);
	cr_assert_eq(observed_core_faults, 0u);
}

Test(aarch64_exception_return, permission_faults_reach_vmm_as_write_protection_faults) {
	struct exception_frame frame = {
		.vector = 8u,
		.esr    = (0x24ull << 26) | (1ull << 6) | 0x0du,
		.far    = 0x12346000u,
	};

	aarch64_exception_test_reset();
	handle_exception(&frame);

	cr_assert_eq(observed_vmm_faults, 1u);
	cr_assert_eq(observed_fault_kind, VMM_FAULT_PROTECTION);
	cr_assert_eq(observed_vmm_access, VMM_FAULT_ACCESS_WRITE);
	cr_assert(observed_vmm_user_mode);
	cr_assert_eq(observed_core_faults, 0u);
}

Test(aarch64_exception_return, data_abort_alignment_is_not_misreported_as_page_protection) {
	struct exception_frame frame = {
		.vector = 8u,
		.esr    = (0x24ull << 26) | 0x21u,
		.far    = 0x12347003u,
	};

	aarch64_exception_test_reset();
	handle_exception(&frame);

	cr_assert_eq(observed_vmm_faults, 0u, "DFSC alignment faults must not enter lazy/protection page-fault handling");
	cr_assert_eq(observed_core_faults, 1u);
	cr_assert_eq(observed_core_kind,
	             CORE_EXCEPTION_ALIGNMENT,
	             "userspace data-abort alignment must retain alignment fault semantics");
}
