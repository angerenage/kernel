#include <core/capability_call.h>
#include <core/id_table.h>
#include <core/lock.h>
#include <core/semaphore.h>
#include <libc/stdlib.h>
#include <stdint.h>

#define CAP_CALL_ID_MIN 1u
#define CAP_CALL_ID_MAX UINT64_MAX
#define CAP_CALL_CANCEL_BATCH 16u

enum cap_pending_state {
	CAP_PENDING_STATE_WAITING = 0,
	CAP_PENDING_STATE_REPLYING,
	CAP_PENDING_STATE_COMPLETED,
};

struct cap_pending_call {
	cap_call_id_t    id;
	channel_id_t     endpoint_id;
	process_id_t     provider;
	process_id_t     caller;
	size_t           response_capacity;
	void*            request;
	size_t           request_size;
	void*            response;
	syscall_result_t result;
	struct semaphore completion;
	uint32_t         state;
	uint32_t         channel_closed;
	uint32_t         caller_closed;
	uint64_t         reference_count;
};

static struct id_table pending_call_table = {
	.lock    = SPINLOCK_INIT_CLASS("pending_call_table", SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),
	.next_id = CAP_CALL_ID_MIN,
	.min_id  = CAP_CALL_ID_MIN,
	.max_id  = CAP_CALL_ID_MAX,
};

static bool cap_pending_call_retain_callback(void* value, void* context) {
	struct cap_pending_call* call = value;
	(void)context;

	(void)__atomic_add_fetch(&call->reference_count, 1u, __ATOMIC_RELAXED);
	return true;
}

static struct cap_pending_call* cap_pending_call_acquire(cap_call_id_t id) {
	if (id == CAP_CALL_ID_INVALID) return NULL;
	return id_table_lookup_retain(&pending_call_table, id, cap_pending_call_retain_callback, NULL);
}

static void cap_pending_call_release(struct cap_pending_call* call) {
	if (call == NULL) return;
	if (__atomic_sub_fetch(&call->reference_count, 1u, __ATOMIC_ACQ_REL) != 0u) return;
	free(call->request);
	free(call->response);
	free(call);
}

static bool cap_pending_call_claim(struct cap_pending_call* call) {
	uint32_t expected = CAP_PENDING_STATE_WAITING;
	return __atomic_compare_exchange_n(
		&call->state, &expected, CAP_PENDING_STATE_REPLYING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void cap_pending_call_complete(struct cap_pending_call* call, syscall_status_t status, size_t response_size) {
	call->result = syscall_result_error(status, response_size);
	if (status == SYSCALL_STATUS_OK) call->result = syscall_result_ok(response_size);
	__atomic_store_n(&call->state, CAP_PENDING_STATE_COMPLETED, __ATOMIC_RELEASE);
	(void)semaphore_release(&call->completion);
}

void cap_pending_call_init(void) {
	(void)id_table_init(&pending_call_table, "pending_call_table", CAP_CALL_ID_MIN, CAP_CALL_ID_MAX);
}

struct cap_pending_call* cap_pending_call_create(channel_id_t endpoint_id, process_id_t provider, process_id_t caller,
                                                 size_t response_capacity) {
	struct cap_pending_call* call;
	id_table_id_t            id;

	if (endpoint_id == CHANNEL_ID_INVALID || provider == PROCESS_PID_INVALID || caller == PROCESS_PID_INVALID) {
		return NULL;
	}
	call = calloc(1u, sizeof(*call));
	if (call == NULL) return NULL;
	if (response_capacity != 0u) {
		call->response = malloc(response_capacity);
		if (call->response == NULL) {
			free(call);
			return NULL;
		}
	}

	call->endpoint_id       = endpoint_id;
	call->provider          = provider;
	call->caller            = caller;
	call->response_capacity = response_capacity;
	call->reference_count   = 1u;
	call->state             = CAP_PENDING_STATE_WAITING;
	semaphore_init(&call->completion, 0u);

	if (id_table_alloc(&pending_call_table, call, &id) != ID_TABLE_OK) {
		cap_pending_call_release(call);
		return NULL;
	}
	call->id = (cap_call_id_t)id;
	return call;
}

cap_call_id_t cap_pending_call_id(const struct cap_pending_call* call) {
	return call == NULL ? CAP_CALL_ID_INVALID : call->id;
}

void cap_pending_call_wait(struct cap_pending_call* call, syscall_result_t* out_result, const void** out_response) {
	if (out_result != NULL) *out_result = syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	if (out_response != NULL) *out_response = NULL;
	if (call == NULL) return;

	if (!semaphore_try_acquire(&call->completion)) semaphore_acquire(&call->completion);
	if (out_result != NULL) *out_result = call->result;
	if (out_response != NULL) *out_response = call->response;
}

void cap_pending_call_destroy(struct cap_pending_call* call) {
	struct cap_pending_call* removed = NULL;

	if (call == NULL) return;
	if (id_table_remove(&pending_call_table, call->id, (void**)&removed) != ID_TABLE_OK) return;
	cap_pending_call_release(removed);
}

bool cap_pending_call_attach_request(struct cap_pending_call* call, void* request, size_t request_size) {
	if (call == NULL || request == NULL || request_size == 0u || call->request != NULL) return false;
	call->request      = request;
	call->request_size = request_size;
	return true;
}

bool cap_pending_call_prepare_receive(cap_call_id_t id, process_id_t provider,
                                      struct cap_pending_request* out_request) {
	struct cap_pending_call* call;

	if (out_request == NULL) return false;
	out_request->call         = NULL;
	out_request->request      = NULL;
	out_request->request_size = 0u;
	call                      = cap_pending_call_acquire(id);
	if (call == NULL) return false;
	if (call->provider != provider || call->request == NULL ||
	    __atomic_load_n(&call->state, __ATOMIC_ACQUIRE) != CAP_PENDING_STATE_WAITING) {
		cap_pending_call_release(call);
		return false;
	}
	out_request->call         = call;
	out_request->request      = call->request;
	out_request->request_size = call->request_size;
	return true;
}

void cap_pending_call_finish_receive(struct cap_pending_request* request) {
	struct cap_pending_call* call;

	if (request == NULL || request->call == NULL) return;
	call                  = request->call;
	request->call         = NULL;
	request->request      = NULL;
	request->request_size = 0u;
	cap_pending_call_release(call);
}

enum cap_pending_reply_result cap_pending_call_prepare_reply(cap_call_id_t id, process_id_t provider,
                                                             size_t                    response_size,
                                                             struct cap_pending_reply* out_reply) {
	struct cap_pending_call* call;

	if (out_reply == NULL) return CAP_PENDING_REPLY_NOT_FOUND;
	out_reply->call     = NULL;
	out_reply->response = NULL;
	call                = cap_pending_call_acquire(id);
	if (call == NULL) return CAP_PENDING_REPLY_NOT_FOUND;
	if (call->provider != provider) {
		cap_pending_call_release(call);
		return CAP_PENDING_REPLY_NOT_OWNER;
	}
	if (response_size > call->response_capacity) {
		cap_pending_call_release(call);
		return CAP_PENDING_REPLY_TOO_LARGE;
	}
	if (!cap_pending_call_claim(call)) {
		cap_pending_call_release(call);
		return CAP_PENDING_REPLY_ALREADY_COMPLETED;
	}

	out_reply->call     = call;
	out_reply->response = call->response;
	return CAP_PENDING_REPLY_OK;
}

void cap_pending_call_finish_reply(struct cap_pending_reply* reply, syscall_status_t status, size_t response_size) {
	struct cap_pending_call* call;

	if (reply == NULL || reply->call == NULL) return;
	call = reply->call;
	cap_pending_call_complete(call, status, response_size);
	reply->call     = NULL;
	reply->response = NULL;
	cap_pending_call_release(call);
}

void cap_pending_call_abort_reply(struct cap_pending_reply* reply) {
	struct cap_pending_call* call;

	if (reply == NULL || reply->call == NULL) return;
	call = reply->call;
	if (__atomic_load_n(&call->channel_closed, __ATOMIC_ACQUIRE) != 0u ||
	    __atomic_load_n(&call->caller_closed, __ATOMIC_ACQUIRE) != 0u) {
		cap_pending_call_complete(call, SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	else {
		__atomic_store_n(&call->state, CAP_PENDING_STATE_WAITING, __ATOMIC_RELEASE);
	}
	reply->call     = NULL;
	reply->response = NULL;
	cap_pending_call_release(call);
}

bool cap_pending_call_fail(cap_call_id_t id, syscall_status_t status) {
	struct cap_pending_call* call = cap_pending_call_acquire(id);

	if (call == NULL) return false;
	if (!cap_pending_call_claim(call)) {
		cap_pending_call_release(call);
		return false;
	}
	cap_pending_call_complete(call, status, 0u);
	cap_pending_call_release(call);
	return true;
}

void cap_pending_call_cancel_channel(channel_id_t endpoint_id) {
	size_t cursor = 0u;

	if (endpoint_id == CHANNEL_ID_INVALID) return;
	for (;;) {
		struct cap_pending_call* batch[CAP_CALL_CANCEL_BATCH];
		size_t                   count = 0u;
		bool                     finished;
		struct irq_state         irq_state = spinlock_lock_irqsave(&pending_call_table.lock);

		while (cursor < pending_call_table.capacity && count < CAP_CALL_CANCEL_BATCH) {
			struct cap_pending_call* call = pending_call_table.slots[cursor++];
			if (call == NULL || call->endpoint_id != endpoint_id ||
			    __atomic_load_n(&call->state, __ATOMIC_ACQUIRE) == CAP_PENDING_STATE_COMPLETED) {
				continue;
			}
			(void)__atomic_add_fetch(&call->reference_count, 1u, __ATOMIC_RELAXED);
			batch[count++] = call;
		}
		finished = cursor >= pending_call_table.capacity;
		spinlock_unlock_irqrestore(&pending_call_table.lock, irq_state);

		for (size_t i = 0u; i < count; i++) {
			__atomic_store_n(&batch[i]->channel_closed, 1u, __ATOMIC_RELEASE);
			if (cap_pending_call_claim(batch[i])) {
				cap_pending_call_complete(batch[i], SYSCALL_STATUS_UNAVAILABLE, 0u);
			}
			cap_pending_call_release(batch[i]);
		}
		if (finished) return;
	}
}

void cap_pending_call_cancel_caller(process_id_t caller) {
	size_t cursor = 0u;

	if (caller == PROCESS_PID_INVALID) return;
	for (;;) {
		struct cap_pending_call* batch[CAP_CALL_CANCEL_BATCH];
		size_t                   count = 0u;
		bool                     finished;
		struct irq_state         irq_state = spinlock_lock_irqsave(&pending_call_table.lock);

		while (cursor < pending_call_table.capacity && count < CAP_CALL_CANCEL_BATCH) {
			struct cap_pending_call* call = pending_call_table.slots[cursor++];
			if (call == NULL || call->caller != caller ||
			    __atomic_load_n(&call->state, __ATOMIC_ACQUIRE) == CAP_PENDING_STATE_COMPLETED) {
				continue;
			}
			(void)__atomic_add_fetch(&call->reference_count, 1u, __ATOMIC_RELAXED);
			batch[count++] = call;
		}
		finished = cursor >= pending_call_table.capacity;
		spinlock_unlock_irqrestore(&pending_call_table.lock, irq_state);

		for (size_t i = 0u; i < count; i++) {
			__atomic_store_n(&batch[i]->caller_closed, 1u, __ATOMIC_RELEASE);
			if (cap_pending_call_claim(batch[i])) {
				cap_pending_call_complete(batch[i], SYSCALL_STATUS_UNAVAILABLE, 0u);
			}
			cap_pending_call_release(batch[i]);
		}
		if (finished) return;
	}
}
