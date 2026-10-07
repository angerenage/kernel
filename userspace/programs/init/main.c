#include <system/display.h>

#include "init.h"
#include "serial_stream.h"
#include "server.h"

int main() {
	if (serial_stream_create(g_init.serial_cap, &g_init.serial_stream_cap) != SYSCALL_STATUS_OK) return 1;
	display_set_standard_streams(CAP_ID_INVALID, g_init.serial_stream_cap, g_init.serial_stream_cap);
	if (!server_init()) return 1;
	int result = server_run(&g_init);
	server_deinit();
	return result;
}
