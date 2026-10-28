#include <stdint.h>
#include <system/trace.h>

#include "syscall.h"

#define TRACE_LABEL_UNAVAILABLE UINT64_MAX

static syscall_status_t trace_label_resolve(struct trace_label* label, trace_label_id_t* out_id) {
	trace_label_id_t id;
	syscall_result_t result;

	if (label == NULL || out_id == NULL || label->name == NULL || label->length == 0u ||
	    label->length > TRACE_LABEL_NAME_MAX) {
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	id = __atomic_load_n(&label->id, __ATOMIC_ACQUIRE);
	if (id == TRACE_LABEL_UNAVAILABLE) return SYSCALL_STATUS_UNAVAILABLE;
	if (id == TRACE_LABEL_ID_INVALID) {
		result = syscall(SYSCALL_TRACE_REGISTER, (uintptr_t)label->name, label->length, 0u, 0u, 0u, 0u);
		if (result.status != SYSCALL_STATUS_OK) {
			trace_label_id_t expected = TRACE_LABEL_ID_INVALID;
			if (result.status == SYSCALL_STATUS_UNAVAILABLE || result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
				(void)__atomic_compare_exchange_n(
					&label->id, &expected, TRACE_LABEL_UNAVAILABLE, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
			}
			return result.status;
		}
		id                        = (trace_label_id_t)result.value;
		trace_label_id_t expected = TRACE_LABEL_ID_INVALID;
		if (!__atomic_compare_exchange_n(&label->id, &expected, id, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
			id = expected;
		}
	}
	if (id == TRACE_LABEL_UNAVAILABLE || id == TRACE_LABEL_ID_INVALID) return SYSCALL_STATUS_UNAVAILABLE;
	*out_id = id;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t trace_emit(struct trace_label* label, enum trace_event_type type) {
	trace_label_id_t label_id;
	syscall_status_t status = trace_label_resolve(label, &label_id);
	if (status != SYSCALL_STATUS_OK) return status;

	syscall_result_t result = syscall(SYSCALL_TRACE_EVENT, (uintptr_t)type, (uintptr_t)label_id, 0u, 0u, 0u, 0u);
	if (result.status == SYSCALL_STATUS_UNAVAILABLE) {
		__atomic_store_n(&label->id, TRACE_LABEL_UNAVAILABLE, __ATOMIC_RELEASE);
	}
	return result.status;
}

syscall_status_t trace_begin(struct trace_label* label) {
	return trace_emit(label, TRACE_EVENT_BEGIN);
}

syscall_status_t trace_end(struct trace_label* label) {
	return trace_emit(label, TRACE_EVENT_END);
}

syscall_status_t trace_instant(struct trace_label* label) {
	return trace_emit(label, TRACE_EVENT_INSTANT);
}

struct trace_scope trace_scope_open(struct trace_label* label) {
	return (struct trace_scope){
		.label  = label,
		.active = trace_begin(label) == SYSCALL_STATUS_OK,
	};
}

void trace_scope_close(struct trace_scope* scope) {
	if (scope == NULL || !scope->active) return;
	(void)trace_end(scope->label);
	scope->active = false;
}
