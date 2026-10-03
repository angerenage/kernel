#include <base/cap.h>
#include <stddef.h>

#include "server.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argc;
	(void)argv;
	(void)capc;
	(void)capv;
	return loader_server_run();
}
