#include "init.h"
#include "server.h"

int main() {
	if (!server_init()) return 1;
	int result = server_run(&g_init);
	server_deinit();
	return result;
}
