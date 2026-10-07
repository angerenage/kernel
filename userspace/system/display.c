#include <runtime/stream.h>
#include <stddef.h>
#include <stdint.h>

static cap_id_t stdin_stream_cap  = CAP_ID_INVALID;
static cap_id_t stdout_stream_cap = CAP_ID_INVALID;
static cap_id_t stderr_stream_cap = CAP_ID_INVALID;

void display_set_standard_streams(cap_id_t stdin_cap, cap_id_t stdout_cap, cap_id_t stderr_cap) {
	stdin_stream_cap  = stdin_cap;
	stdout_stream_cap = stdout_cap;
	stderr_stream_cap = stderr_cap;
}

syscall_status_t display_read(void* data, size_t capacity, size_t* out_read) {
	if (out_read != NULL) *out_read = 0u;
	if (stdin_stream_cap == CAP_ID_INVALID) return SYSCALL_STATUS_UNAVAILABLE;
	return stream_read(stdin_stream_cap, data, capacity, out_read);
}

static syscall_status_t display_write_stream(cap_id_t stream_cap, const char* data, size_t length) {
	size_t offset = 0u;

	if (stream_cap == CAP_ID_INVALID) return SYSCALL_STATUS_UNAVAILABLE;
	if (length != 0u && data == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	while (offset < length) {
		size_t           written = 0u;
		syscall_status_t status  = stream_write(stream_cap, data + offset, length - offset, &written);
		if (status != SYSCALL_STATUS_OK) return status;
		if (written == 0u || written > length - offset) return SYSCALL_STATUS_FAILED;
		offset += written;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t display_write(const char* data, size_t length) {
	return display_write_stream(stdout_stream_cap, data, length);
}

syscall_status_t display_error_write(const char* data, size_t length) {
	return display_write_stream(stderr_stream_cap, data, length);
}
