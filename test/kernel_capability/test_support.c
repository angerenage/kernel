#include "test_support.h"

#include <hal/cache.h>
#include <test_memory.h>

#include "../../kernel/src/capability/memory_allocator.h"
#include "../../kernel/src/syscall/capability.h"

static size_t          serial_bytes;
static cap_object_id_t loader_object_id = CAP_OBJECT_ID_INVALID;

static syscall_result_t loader_test_handler(const struct cap_request* request) {
	(void)request;
	return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
}

void kernel_capability_loader_init(void) {
	loader_object_id = cap_object_create_kernel(0u, loader_test_handler, NULL);
}

bool kernel_capability_loader_available(void) {
	return loader_object_id != CAP_OBJECT_ID_INVALID;
}

cap_id_t kernel_capability_loader_grant(process_id_t recipient) {
	if (loader_object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
	return cap_create(loader_object_id, recipient, CAP_CALL | CAP_DELEGATE, NULL);
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
	cr_assert(kernel_memory_allocator_init());
	kernel_boot_mock_reset();
	kernel_capability_test_serial_reset();

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

uintptr_t kernel_capability_test_alloc_user_buffer(struct process* process, size_t size, struct mapping** out_mapping) {
	void* base = NULL;

	cr_assert_not_null(process);
	cr_assert_not_null(out_mapping);
	*out_mapping = NULL;
	cr_assert(test_address_space_map(process_address_space(process),
	                                 size,
	                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                                 0u,
	                                 TEST_MAPPING_GRANULE,
	                                 0u,
	                                 out_mapping,
	                                 &base));
	cr_assert_not_null(base);
	return (uintptr_t)base;
}

void kernel_capability_test_poison_next_pmm_page(uint8_t value) {
	struct pmm_extent allocation;

	cr_assert(
		pmm_alloc(&(const struct pmm_alloc_request){.size = TEST_MAPPING_GRANULE, .alignment = TEST_MAPPING_GRANULE},
	              &allocation));
	memset((void*)(allocation.address + boot_info.direct_map_offset), value, allocation.size);
	cr_assert(pmm_free(allocation), "failed to return poisoned PMM extent");
}
