#pragma once

#include <base/cap.h>
#include <base/record.h>
#include <base/syscall.h>
#include <stddef.h>

/* Decoded operation shared by record capability providers. */
struct kernel_record_request {
	enum record_op op;
	size_t         index;
};

/* Validate and decode a request for records of one fixed size. */
syscall_result_t kernel_record_request_decode(const struct cap_request* request, size_t record_size,
                                              struct kernel_record_request* out_request);

/* Write a record count after successful request decoding. */
syscall_result_t kernel_record_count_respond(const struct cap_request* request, size_t count);
