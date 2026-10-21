#include <system/display.h>

#include "init.h"
#include "launcher.h"
#include "serial_stream.h"
#include "supervisor.h"

int main() {
	if (serial_stream_create(g_init.serial_cap, &g_init.serial_stream_cap) != SYSCALL_STATUS_OK) return 1;
	display_set_standard_streams(CAP_ID_INVALID, g_init.serial_stream_cap, g_init.serial_stream_cap);
	if (!bootstrap_launch_registry(&g_init)) return 1;
	return supervisor_run(&g_init);
}
