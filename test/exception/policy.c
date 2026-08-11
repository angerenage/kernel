#include <base/process.h>
#include <core/exception.h>
#include <core/process.h>
#include <core/vmm.h>
#include <criterion/criterion.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static struct process* fake_current_process = (struct process*)(uintptr_t)0x1000u;
static bool            terminate_result;
static size_t          terminate_calls;
static struct process* terminated_process;
static uintptr_t       terminated_exit_code;

static bool                  vmm_fault_result;
static size_t                vmm_fault_calls;
static uintptr_t             observed_fault_addr;
static enum vmm_fault_kind   observed_fault_kind;
static enum vmm_fault_access observed_fault_access;
static bool                  observed_fault_user_mode;

struct process* process_current(void) {
	return fake_current_process;
}

bool process_terminate(struct process* process, uintptr_t exit_code) {
	terminate_calls++;
	terminated_process   = process;
	terminated_exit_code = exit_code;
	return terminate_result;
}

bool vmm_handle_current_page_fault(uintptr_t addr, enum vmm_fault_kind kind, enum vmm_fault_access access,
                                   bool user_mode) {
	vmm_fault_calls++;
	observed_fault_addr      = addr;
	observed_fault_kind      = kind;
	observed_fault_access    = access;
	observed_fault_user_mode = user_mode;
	return vmm_fault_result;
}

static void exception_test_reset(void) {
	fake_current_process     = (struct process*)(uintptr_t)0x1000u;
	terminate_result         = true;
	terminate_calls          = 0u;
	terminated_process       = NULL;
	terminated_exit_code     = UINTPTR_MAX;
	vmm_fault_result         = true;
	vmm_fault_calls          = 0u;
	observed_fault_addr      = 0u;
	observed_fault_kind      = VMM_FAULT_INVALID;
	observed_fault_access    = VMM_FAULT_ACCESS_UNKNOWN;
	observed_fault_user_mode = false;
}

Test(exception_core, page_faults_preserve_kind_access_address_and_origin) {
	static const struct {
		enum core_exception_kind   core_kind;
		enum core_exception_access core_access;
		enum vmm_fault_kind        vmm_kind;
		enum vmm_fault_access      vmm_access;
		bool                       user_mode;
	} cases[] = {
		{
         .core_kind   = CORE_EXCEPTION_PAGE_FAULT_NOT_PRESENT,
         .core_access = CORE_EXCEPTION_ACCESS_READ,
         .vmm_kind    = VMM_FAULT_NOT_PRESENT,
         .vmm_access  = VMM_FAULT_ACCESS_READ,
         .user_mode   = true,
		 },
		{
         .core_kind   = CORE_EXCEPTION_PAGE_FAULT_PROTECTION,
         .core_access = CORE_EXCEPTION_ACCESS_WRITE,
         .vmm_kind    = VMM_FAULT_PROTECTION,
         .vmm_access  = VMM_FAULT_ACCESS_WRITE,
         .user_mode   = true,
		 },
		{
         .core_kind   = CORE_EXCEPTION_PAGE_FAULT_INVALID,
         .core_access = CORE_EXCEPTION_ACCESS_EXEC,
         .vmm_kind    = VMM_FAULT_INVALID,
         .vmm_access  = VMM_FAULT_ACCESS_EXEC,
         .user_mode   = false,
		 },
	};

	for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const uintptr_t addr = 0x4000u + i * 0x1000u;

		exception_test_reset();
		vmm_fault_result = (i & 1u) == 0u;

		cr_assert_eq(core_handle_exception(cases[i].core_kind, cases[i].core_access, addr, cases[i].user_mode),
		             vmm_fault_result);
		cr_assert_eq(vmm_fault_calls, 1u);
		cr_assert_eq(observed_fault_addr, addr);
		cr_assert_eq(observed_fault_kind, cases[i].vmm_kind);
		cr_assert_eq(observed_fault_access, cases[i].vmm_access);
		cr_assert_eq(observed_fault_user_mode, cases[i].user_mode);
		cr_assert_eq(terminate_calls, 0u, "page faults must stay delegated to the VMM policy");
	}
}

Test(exception_core, public_user_faults_publish_their_stable_process_exit_codes) {
	static const struct {
		enum core_exception_kind kind;
		uintptr_t                exit_code;
	} cases[] = {
		{   CORE_EXCEPTION_ARITHMETIC_DIVIDE_BY_ZERO,    PROCESS_EXIT_ARITHMETIC_DIVIDE_BY_ZERO},
		{		 CORE_EXCEPTION_ARITHMETIC_OVERFLOW,          PROCESS_EXIT_ARITHMETIC_OVERFLOW},
		{      CORE_EXCEPTION_ARITHMETIC_BOUND_RANGE,       PROCESS_EXIT_ARITHMETIC_BOUND_RANGE},
		{		 CORE_EXCEPTION_INSTRUCTION_ILLEGAL,          PROCESS_EXIT_INSTRUCTION_ILLEGAL},
		{CORE_EXCEPTION_PRIVILEGE_GENERAL_PROTECTION, PROCESS_EXIT_PRIVILEGE_GENERAL_PROTECTION},
		{				   CORE_EXCEPTION_ALIGNMENT,              PROCESS_EXIT_ALIGNMENT_FAULT},
		{    CORE_EXCEPTION_ACCESS_INSTRUCTION_ABORT,     PROCESS_EXIT_ACCESS_INSTRUCTION_ABORT},
		{		   CORE_EXCEPTION_ACCESS_DATA_ABORT,            PROCESS_EXIT_ACCESS_DATA_ABORT},
		{  CORE_EXCEPTION_ACCESS_ADDRESS_ERROR_FETCH,   PROCESS_EXIT_ACCESS_ADDRESS_ERROR_FETCH},
		{ CORE_EXCEPTION_ACCESS_ADDRESS_ERROR_MEMORY,  PROCESS_EXIT_ACCESS_ADDRESS_ERROR_MEMORY},
		{				   CORE_EXCEPTION_BUS_ERROR,                    PROCESS_EXIT_BUS_ERROR},
		{			  CORE_EXCEPTION_FLOATING_POINT,         PROCESS_EXIT_FLOATING_POINT_ERROR},
		{		 CORE_EXCEPTION_FLOATING_POINT_SIMD,          PROCESS_EXIT_FLOATING_POINT_SIMD},
		{		   CORE_EXCEPTION_MEMORY_PROTECTION,            PROCESS_EXIT_MEMORY_PROTECTION},
	};

	for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
		exception_test_reset();

		cr_assert(
			core_handle_user_exception(cases[i].kind), "user exception kind %d was not consumed", (int)cases[i].kind);
		cr_assert_eq(terminate_calls, 1u);
		cr_assert_eq(terminated_process, fake_current_process);
		cr_assert_eq(terminated_exit_code, cases[i].exit_code);
	}
}

Test(exception_core, debugger_generated_user_traps_are_contained_by_the_core) {
	static const struct {
		enum core_exception_kind kind;
		uintptr_t                exit_code;
	} cases[] = {
		{CORE_EXCEPTION_BREAKPOINT, PROCESS_EXIT_BREAKPOINT},
		{     CORE_EXCEPTION_DEBUG,      PROCESS_EXIT_DEBUG},
		{CORE_EXCEPTION_WATCHPOINT, PROCESS_EXIT_WATCHPOINT},
	};

	for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
		exception_test_reset();

		cr_assert(core_handle_user_exception(cases[i].kind),
		          "userspace trap kind %d escaped to architecture-fatal handling",
		          (int)cases[i].kind);
		cr_assert_eq(terminate_calls, 1u);
		cr_assert_eq(terminated_exit_code, cases[i].exit_code);
	}
}

Test(exception_core, kernel_non_page_exceptions_are_left_to_architecture_fatal_handling) {
	exception_test_reset();

	cr_assert_not(core_handle_exception(CORE_EXCEPTION_INSTRUCTION_ILLEGAL, CORE_EXCEPTION_ACCESS_UNKNOWN, 0u, false));
	cr_assert_eq(vmm_fault_calls, 0u);
	cr_assert_eq(terminate_calls, 0u);
}

Test(exception_core, user_exception_without_a_current_process_is_not_falsely_reported_handled) {
	exception_test_reset();
	fake_current_process = NULL;

	cr_assert_not(core_handle_user_exception(CORE_EXCEPTION_INSTRUCTION_ILLEGAL));
	cr_assert_eq(terminate_calls, 0u);
}
