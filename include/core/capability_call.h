#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/semaphore.h>
#include <stdbool.h>
#include <stddef.h>

/* One userspace-provider call retained until reply or cancellation. */
struct cap_pending_call {
	cap_call_id_t      id;
	channel_id_t       endpoint_id;
	process_id_t       provider;
	process_id_t       caller;
	size_t             response_capacity;
	void*              request;
	size_t             request_size;
	void*              response;
	syscall_result_t   result;
	struct semaphore   completion;
	struct cap_object* object;
	uint32_t           state;
	uint32_t           channel_closed;
	uint32_t           caller_closed;
	uint64_t           reference_count;
};

/* A claimed provider reply and its writable response buffer. */
struct cap_pending_reply {
	struct cap_pending_call* call;
	void*                    response;
};

/* A pinned provider request and its immutable request buffer. */
struct cap_pending_request {
	struct cap_pending_call* call;
	const void*              request;
	size_t                   request_size;
};

/* Results from claiming a pending call for reply. */
enum cap_pending_reply_result {
	CAP_PENDING_REPLY_OK = 0,
	CAP_PENDING_REPLY_NOT_FOUND,
	CAP_PENDING_REPLY_NOT_OWNER,
	CAP_PENDING_REPLY_TOO_LARGE,
	CAP_PENDING_REPLY_ALREADY_COMPLETED,
};

/* Initialize the pending-call registry. */
void cap_pending_call_init(void);

/* Create a pending provider call and take ownership of its active object reference. */
struct cap_pending_call* cap_pending_call_create(struct cap_object* object, channel_id_t endpoint_id,
                                                 process_id_t provider, process_id_t caller, size_t response_capacity);

/* Return the stable identifier of a pending call. */
cap_call_id_t cap_pending_call_id(const struct cap_pending_call* call);

/* Wait for a pending call to reach a terminal result. */
void cap_pending_call_wait(struct cap_pending_call* call, syscall_result_t* out_result, const void** out_response);

/* Remove and release a pending call. */
void cap_pending_call_destroy(struct cap_pending_call* call);

/* Transfer ownership of a kernel request buffer to call before publishing its queue entry. */
bool cap_pending_call_attach_request(struct cap_pending_call* call, void* request, size_t request_size);

/*
 * Pin a queued request buffer before dereferencing it. This rejects canceled or already-removed calls, allowing stale
 * queue entries to be discarded without touching their old pointer value.
 */
bool cap_pending_call_prepare_receive(cap_call_id_t id, process_id_t provider, struct cap_pending_request* out_request);

/* Release the request-buffer pin established by cap_pending_call_prepare_receive(). */
void cap_pending_call_finish_receive(struct cap_pending_request* request);

/* Claim a pending call for a provider reply. */
enum cap_pending_reply_result cap_pending_call_prepare_reply(cap_call_id_t id, process_id_t provider,
                                                             size_t response_size, struct cap_pending_reply* out_reply);

/* Publish the terminal result of a claimed provider reply. */
void cap_pending_call_finish_reply(struct cap_pending_reply* reply, syscall_status_t status, size_t response_size);

/* Release a reply claim so it can be retried or canceled. */
void cap_pending_call_abort_reply(struct cap_pending_reply* reply);

/* Complete one pending call with an error status. */
bool cap_pending_call_fail(cap_call_id_t id, syscall_status_t status);

/* Cancel every pending call routed through an endpoint. */
void cap_pending_call_cancel_channel(channel_id_t endpoint_id);

/* Cancel every pending call made by a caller. */
void cap_pending_call_cancel_caller(process_id_t caller);

/* Complete every outstanding request serviced by provider when that process terminates. */
void cap_pending_call_cancel_provider(process_id_t provider);
