#include "process.h"

#include <base/self.h>
#include <core/capability.h>
#include <core/message.h>
#include <core/syscall.h>
#include <core/uthread.h>

static syscall_result_t self_handler(const struct cap_request* req) {
	struct process*     process;
	struct uthread*     thread;
	struct self_info    info;
	struct process*     caller;
	enum message_result result;

	process = (struct process*)(uintptr_t)req->object_id;
	if (process == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	thread = uthread_current();

	info.pid          = process_pid(process);
	info.thread_id    = thread != NULL ? (uint64_t)uthread_id(thread) : 0u;
	info.thread_count = process_thread_count(process);
	info.self_cap     = process->self_cap;

	caller = process_lookup(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	result = message_queue_send(&caller->message_queue, req->caller, &info, sizeof(info));
	if (result != MESSAGE_OK) return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);

	return syscall_result_ok(0u);
}

cap_id_t kernel_self_grant(struct process* process) {
	struct cap_object* object;
	struct capability* cap;

	if (process == NULL) return CAP_ID_INVALID;
	if (process->self_cap != CAP_ID_INVALID) return process->self_cap;

	object = cap_object_create_kernel((uint64_t)(uintptr_t)process, self_handler);
	if (object == NULL) return CAP_ID_INVALID;

	cap = cap_create(object, process_pid(process), CAP_CALL | CAP_DELEGATE, NULL);
	if (cap == NULL) {
		cap_object_destroy(object);
		return CAP_ID_INVALID;
	}

	process->self_cap = cap->cap_id;
	return cap->cap_id;
}
