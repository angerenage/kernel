#include "../../kernel/src/capability/serial.h"

#include "test_support.h"

Test(kernel_capability_serial, call_right_does_not_implicitly_grant_serial_write) {
	struct kernel_capability_test_context ctx;
	struct capability*                    root;
	struct capability*                    call_only;
	cap_id_t                              root_id;
	syscall_result_t                      result;
	const char                            payload[] = "must-not-write";

	kernel_capability_test_begin(&ctx, "kernel-cap/serial-rights");
	kernel_capability_serial_init();
	root_id = kernel_capability_serial_grant(process_pid(ctx.process));
	cr_assert_neq(root_id, CAP_ID_INVALID);

	root = cap_acquire(root_id);
	cr_assert_not_null(root);
	call_only = cap_create(root->cap_object_id, process_pid(ctx.process), CAP_CALL, root, NULL);
	cr_assert_not_null(call_only);
	cap_release(root);

	kernel_capability_test_serial_reset();
	result = kernel_capability_test_call(call_only->cap_id, payload, sizeof(payload), NULL, 0u);
	cr_assert_eq(
		result.status, SYSCALL_STATUS_DENIED, "CAP_CALL-only serial capability bypassed the service's CAP_WRITE right");
	cr_assert_eq(kernel_capability_test_serial_bytes(), 0u, "denied serial call emitted bytes");

	kernel_capability_test_end(&ctx);
}
