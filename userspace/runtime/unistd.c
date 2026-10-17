#include <libc/errno.h>
#include <libc/stdio.h>
#include <libc/unistd.h>
#include <system/display.h>

ssize_t read(int descriptor, void* buffer, size_t count) {
	if (descriptor != 0) {
		errno = EBADF;
		return -1;
	}
	if (buffer == NULL && count != 0u) {
		errno = EINVAL;
		return -1;
	}
	if (count == 0u) return 0;
	size_t           result = 0u;
	syscall_status_t status = display_read(buffer, count, &result);
	if (status != SYSCALL_STATUS_OK) {
		errno = EIO;
		return -1;
	}
	return (ssize_t)result;
}

ssize_t write(int descriptor, const void* buffer, size_t count) {
	FILE* stream;
	if (descriptor == 1) stream = stdout;
	else if (descriptor == 2) stream = stderr;
	else {
		errno = EBADF;
		return -1;
	}
	if (buffer == NULL && count != 0u) {
		errno = EINVAL;
		return -1;
	}
	size_t result = fwrite(buffer, 1u, count, stream);
	if (result == 0u && count != 0u && ferror(stream)) {
		errno = EIO;
		return -1;
	}
	return (ssize_t)result;
}
