#include "test_support.h"

#include "../../kernel/src/syscall/capability.h"

static size_t serial_bytes;
static size_t executable_sync_count;

void kernel_capability_loader_init(void) {
}

void hal_serial_init(void) {
}

void hal_serial_write_char(char ch) {
	(void)ch;
	serial_bytes++;
}

void hal_serial_write(const char* data, size_t length) {
	if (data == NULL) return;
	serial_bytes += length;
}

void hal_paging_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
	executable_sync_count++;
}

void kernel_capability_test_serial_reset(void) {
	serial_bytes = 0u;
}

size_t kernel_capability_test_serial_bytes(void) {
	return serial_bytes;
}

void kernel_capability_test_begin(struct kernel_capability_test_context* ctx, const char* name) {
	cr_assert_not_null(ctx);
	*ctx = (struct kernel_capability_test_context){0};

	syscall_test_init_process_environment();
	capability_init();
	kernel_boot_mock_reset();
	kernel_capability_test_serial_reset();
	executable_sync_count = 0u;

	ctx->process = syscall_test_spawn_process(name);
	cr_assert_not_null(ctx->process);
	ctx->main_thread = process_main_thread(ctx->process);
	cr_assert_not_null(ctx->main_thread);
	sched_set_current(cpu_current(), &ctx->main_thread->thread);

	/* Kernel-capability calls below pass hosted pointers for their request/response envelope. */
	ctx->main_thread->thread.address_space = NULL;
}

void kernel_capability_test_end(struct kernel_capability_test_context* ctx) {
	if (ctx == NULL || ctx->process == NULL) return;

	thread_mark_zombie(&ctx->main_thread->thread);
	sched_set_current(cpu_current(), NULL);
	cr_assert(process_destroy(ctx->process), "failed to destroy kernel capability test process");
	kernel_boot_mock_reset();
	syscall_test_reset_state();
	*ctx = (struct kernel_capability_test_context){0};
}

syscall_result_t kernel_capability_test_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                                             size_t response_capacity) {
	return syscall_cap_call((uintptr_t)cap,
	                        (uintptr_t)request,
	                        (uintptr_t)request_size,
	                        (uintptr_t)response,
	                        (uintptr_t)response_capacity,
	                        0u);
}

uintptr_t kernel_capability_test_alloc_user_buffer(struct process* process, size_t page_count, vmm_id_t* out_id) {
	void* base = NULL;

	cr_assert_not_null(process);
	cr_assert_not_null(out_id);
	*out_id = VMM_ID_INVALID;
	cr_assert(test_vm_map(
		process_address_space(process), page_count, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, out_id, &base));
	cr_assert_not_null(base);
	return (uintptr_t)base;
}

void kernel_capability_test_poison_next_pmm_page(uint8_t value) {
	uintptr_t phys = 0u;

	cr_assert(pmm_alloc_pages(1u, &phys), "failed to reserve PMM page for poisoning");
	memset((void*)(phys + boot_info.direct_map_offset), value, PMM_PAGE_SIZE);
	cr_assert(pmm_free_pages(phys, 1u), "failed to return poisoned PMM page");
}

size_t kernel_capability_test_executable_sync_count(void) {
	return executable_sync_count;
}
