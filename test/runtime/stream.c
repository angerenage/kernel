#include <base/cap.h>
#include <base/syscall.h>
#include <criterion/criterion.h>
#include <runtime/stream.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct cap_call_mock {
	syscall_status_t status;
	cap_id_t         cap;
	size_t           request_size;
	size_t           response_capacity;
	size_t           response_size;
	size_t           calls;
	uint8_t          request[CAP_MAX_REQUEST_SIZE];
	uint8_t          response[CAP_MAX_RESPONSE_SIZE];
};

static struct cap_call_mock cap_call_mock;

static void cap_call_mock_reset(void) {
	memset(&cap_call_mock, 0, sizeof(cap_call_mock));
	cap_call_mock.status = SYSCALL_STATUS_OK;
}

static void cap_call_mock_write_response(uint64_t size, size_t response_size) {
	const struct stream_write_response response = {.size = size};

	memcpy(cap_call_mock.response, &response, sizeof(response));
	cap_call_mock.response_size = response_size;
}

syscall_status_t cap_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                          size_t response_capacity, size_t* result_value) {
	size_t copy_size;

	cap_call_mock.calls++;
	cap_call_mock.cap               = cap;
	cap_call_mock.request_size      = request_size;
	cap_call_mock.response_capacity = response_capacity;
	copy_size = request_size < sizeof(cap_call_mock.request) ? request_size : sizeof(cap_call_mock.request);
	if (copy_size != 0u && request != NULL) memcpy(cap_call_mock.request, request, copy_size);
	if (cap_call_mock.status != SYSCALL_STATUS_OK) return cap_call_mock.status;

	copy_size = cap_call_mock.response_size < response_capacity ? cap_call_mock.response_size : response_capacity;
	if (copy_size != 0u && response != NULL) memcpy(response, cap_call_mock.response, copy_size);
	if (result_value != NULL) *result_value = cap_call_mock.response_size;
	return SYSCALL_STATUS_OK;
}

static struct stream_read_request captured_read_request(void) {
	struct stream_read_request request;

	memcpy(&request, cap_call_mock.request, sizeof(request));
	return request;
}

static void captured_write_request(struct stream_write_request* out_request) {
	memcpy(out_request, cap_call_mock.request, sizeof(*out_request));
}

Test(runtime_stream, read_reports_full_partial_and_end_of_stream) {
	uint8_t                    buffer[4] = {0};
	size_t                     read      = SIZE_MAX;
	struct stream_read_request request;

	cap_call_mock_reset();
	memcpy(cap_call_mock.response, "data", sizeof(buffer));
	cap_call_mock.response_size = sizeof(buffer);
	cr_assert_eq(stream_read(7u, buffer, sizeof(buffer), &read), SYSCALL_STATUS_OK);
	cr_assert_eq(read, sizeof(buffer));
	cr_assert_arr_eq(buffer, "data", sizeof(buffer));
	request = captured_read_request();
	cr_assert_eq(cap_call_mock.calls, 1u);
	cr_assert_eq(cap_call_mock.cap, 7u);
	cr_assert_eq(cap_call_mock.request_size, sizeof(request));
	cr_assert_eq(cap_call_mock.response_capacity, sizeof(buffer));
	cr_assert_eq(request.header.op, STREAM_OP_READ);
	cr_assert_eq(request.reserved, 0u);
	cr_assert_eq(request.size, sizeof(buffer));

	cap_call_mock_reset();
	memcpy(cap_call_mock.response, "xy", 2u);
	cap_call_mock.response_size = 2u;
	read                        = SIZE_MAX;
	cr_assert_eq(stream_read(7u, buffer, sizeof(buffer), &read), SYSCALL_STATUS_OK);
	cr_assert_eq(read, 2u);
	cr_assert_arr_eq(buffer, "xy", 2u);

	cap_call_mock_reset();
	read = SIZE_MAX;
	cr_assert_eq(stream_read(7u, buffer, sizeof(buffer), &read), SYSCALL_STATUS_OK);
	cr_assert_eq(read, 0u);
}

Test(runtime_stream, read_clamps_to_the_transport_limit_and_sends_zero_length_calls) {
	static uint8_t             buffer[CAP_MAX_RESPONSE_SIZE + 1u];
	struct stream_read_request request;
	size_t                     read = SIZE_MAX;

	cap_call_mock_reset();
	cap_call_mock.response[0]   = 0x5au;
	cap_call_mock.response_size = 1u;
	cr_assert_eq(stream_read(9u, buffer, sizeof(buffer), &read), SYSCALL_STATUS_OK);
	cr_assert_eq(read, 1u);
	request = captured_read_request();
	cr_assert_eq(request.size, CAP_MAX_RESPONSE_SIZE);
	cr_assert_eq(cap_call_mock.response_capacity, CAP_MAX_RESPONSE_SIZE);

	cap_call_mock_reset();
	read = SIZE_MAX;
	cr_assert_eq(stream_read(9u, NULL, 0u, &read), SYSCALL_STATUS_OK);
	cr_assert_eq(read, 0u);
	cr_assert_eq(cap_call_mock.calls, 1u);
	cr_assert_eq(cap_call_mock.response_capacity, 0u);
	request = captured_read_request();
	cr_assert_eq(request.size, 0u);
}

Test(runtime_stream, read_validates_arguments_errors_and_response_size) {
	uint8_t buffer[2];
	size_t  read = SIZE_MAX;

	cap_call_mock_reset();
	cr_assert_eq(stream_read(CAP_ID_INVALID, buffer, sizeof(buffer), &read), SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(read, 0u);
	cr_assert_eq(cap_call_mock.calls, 0u);
	read = SIZE_MAX;
	cr_assert_eq(stream_read(1u, NULL, 1u, &read), SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(read, 0u);
	cr_assert_eq(cap_call_mock.calls, 0u);
	cr_assert_eq(stream_read(1u, buffer, sizeof(buffer), NULL), SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(cap_call_mock.calls, 0u);

	cap_call_mock_reset();
	cap_call_mock.status = SYSCALL_STATUS_INTERRUPTED;
	read                 = SIZE_MAX;
	cr_assert_eq(stream_read(1u, buffer, sizeof(buffer), &read), SYSCALL_STATUS_INTERRUPTED);
	cr_assert_eq(read, 0u);

	cap_call_mock_reset();
	cap_call_mock.response_size = sizeof(buffer) + 1u;
	read                        = SIZE_MAX;
	cr_assert_eq(stream_read(1u, buffer, sizeof(buffer), &read), SYSCALL_STATUS_FAILED);
	cr_assert_eq(read, 0u);
}

Test(runtime_stream, write_reports_full_partial_and_zero_length_transfers) {
	const uint8_t               data[]  = {1u, 2u, 3u, 4u};
	size_t                      written = SIZE_MAX;
	struct stream_write_request request;

	cap_call_mock_reset();
	cap_call_mock_write_response(sizeof(data), sizeof(struct stream_write_response));
	cr_assert_eq(stream_write(11u, data, sizeof(data), &written), SYSCALL_STATUS_OK);
	cr_assert_eq(written, sizeof(data));
	captured_write_request(&request);
	cr_assert_eq(cap_call_mock.calls, 1u);
	cr_assert_eq(cap_call_mock.cap, 11u);
	cr_assert_eq(cap_call_mock.request_size, sizeof(request) + sizeof(data));
	cr_assert_eq(cap_call_mock.response_capacity, sizeof(struct stream_write_response));
	cr_assert_eq(request.header.op, STREAM_OP_WRITE);
	cr_assert_eq(request.reserved, 0u);
	cr_assert_eq(request.size, sizeof(data));
	cr_assert_arr_eq(cap_call_mock.request + sizeof(request), data, sizeof(data));

	cap_call_mock_reset();
	cap_call_mock_write_response(2u, sizeof(struct stream_write_response));
	written = SIZE_MAX;
	cr_assert_eq(stream_write(11u, data, sizeof(data), &written), SYSCALL_STATUS_OK);
	cr_assert_eq(written, 2u);

	cap_call_mock_reset();
	cap_call_mock_write_response(0u, sizeof(struct stream_write_response));
	written = SIZE_MAX;
	cr_assert_eq(stream_write(11u, NULL, 0u, &written), SYSCALL_STATUS_OK);
	cr_assert_eq(written, 0u);
	cr_assert_eq(cap_call_mock.calls, 1u);
	captured_write_request(&request);
	cr_assert_eq(request.size, 0u);
	cr_assert_eq(cap_call_mock.request_size, sizeof(request));
}

Test(runtime_stream, write_clamps_to_the_transport_limit) {
	enum { MAXIMUM_WRITE_SIZE = CAP_MAX_REQUEST_SIZE - sizeof(struct stream_write_request) };
	static uint8_t              data[MAXIMUM_WRITE_SIZE + 1u];
	struct stream_write_request request;
	size_t                      written = SIZE_MAX;

	for (size_t i = 0u; i < sizeof(data); i++) data[i] = (uint8_t)i;
	cap_call_mock_reset();
	cap_call_mock_write_response(MAXIMUM_WRITE_SIZE, sizeof(struct stream_write_response));
	cr_assert_eq(stream_write(13u, data, sizeof(data), &written), SYSCALL_STATUS_OK);
	cr_assert_eq(written, MAXIMUM_WRITE_SIZE);
	captured_write_request(&request);
	cr_assert_eq(request.size, MAXIMUM_WRITE_SIZE);
	cr_assert_eq(cap_call_mock.request_size, CAP_MAX_REQUEST_SIZE);
	cr_assert_arr_eq(cap_call_mock.request + sizeof(request), data, MAXIMUM_WRITE_SIZE);
}

Test(runtime_stream, write_validates_arguments_errors_and_responses) {
	const uint8_t data[]  = {1u, 2u};
	size_t        written = SIZE_MAX;

	cap_call_mock_reset();
	cr_assert_eq(stream_write(CAP_ID_INVALID, data, sizeof(data), &written), SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(written, 0u);
	cr_assert_eq(cap_call_mock.calls, 0u);
	written = SIZE_MAX;
	cr_assert_eq(stream_write(1u, NULL, 1u, &written), SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(written, 0u);
	cr_assert_eq(cap_call_mock.calls, 0u);
	cr_assert_eq(stream_write(1u, data, sizeof(data), NULL), SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(cap_call_mock.calls, 0u);

	cap_call_mock_reset();
	cap_call_mock.status = SYSCALL_STATUS_DENIED;
	written              = SIZE_MAX;
	cr_assert_eq(stream_write(1u, data, sizeof(data), &written), SYSCALL_STATUS_DENIED);
	cr_assert_eq(written, 0u);

	cap_call_mock_reset();
	cap_call_mock_write_response(sizeof(data), sizeof(struct stream_write_response) - 1u);
	written = SIZE_MAX;
	cr_assert_eq(stream_write(1u, data, sizeof(data), &written), SYSCALL_STATUS_FAILED);
	cr_assert_eq(written, 0u);

	cap_call_mock_reset();
	cap_call_mock_write_response(sizeof(data) + 1u, sizeof(struct stream_write_response));
	cr_assert_eq(stream_write(1u, data, sizeof(data), &written), SYSCALL_STATUS_FAILED);
	cr_assert_eq(written, 0u);

	cap_call_mock_reset();
	cap_call_mock_write_response(0u, sizeof(struct stream_write_response));
	cr_assert_eq(stream_write(1u, data, sizeof(data), &written), SYSCALL_STATUS_FAILED);
	cr_assert_eq(written, 0u);
}
