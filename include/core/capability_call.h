#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stdbool.h>
#include <stddef.h>

struct cap_pending_call;

struct cap_pending_reply {
	struct cap_pending_call* call;
	void*                    response;
};

enum cap_pending_reply_result {
	CAP_PENDING_REPLY_OK = 0,
	CAP_PENDING_REPLY_NOT_FOUND,
	CAP_PENDING_REPLY_NOT_OWNER,
	CAP_PENDING_REPLY_TOO_LARGE,
	CAP_PENDING_REPLY_ALREADY_COMPLETED,
};

void cap_pending_call_init(void);

struct cap_pending_call* cap_pending_call_create(channel_id_t endpoint_id, process_id_t provider, process_id_t caller,
                                                 size_t response_capacity);
cap_call_id_t            cap_pending_call_id(const struct cap_pending_call* call);
void cap_pending_call_wait(struct cap_pending_call* call, syscall_result_t* out_result, const void** out_response);
void cap_pending_call_destroy(struct cap_pending_call* call);

enum cap_pending_reply_result cap_pending_call_prepare_reply(cap_call_id_t id, process_id_t provider,
                                                             size_t response_size, struct cap_pending_reply* out_reply);
void cap_pending_call_finish_reply(struct cap_pending_reply* reply, syscall_status_t status, size_t response_size);
void cap_pending_call_abort_reply(struct cap_pending_reply* reply);

bool cap_pending_call_fail(cap_call_id_t id, syscall_status_t status);
void cap_pending_call_cancel_channel(channel_id_t endpoint_id);
void cap_pending_call_cancel_caller(process_id_t caller);
