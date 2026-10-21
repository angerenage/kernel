#include <base/process.h>
#include <errno.h>
#include <stdlib.h>

#include "server.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	char*              end;
	unsigned long long parsed_pid;
	process_id_t       init_pid;

	if (argc != 1 || argv == NULL || argv[0] == NULL || argv[0][0] == '-' || capc != 1u || capv == NULL ||
	    capv[0] == CAP_ID_INVALID)
		return 1;
	errno      = 0;
	parsed_pid = strtoull(argv[0], &end, 10);
	if (errno != 0 || end == argv[0] || *end != '\0' || parsed_pid == PROCESS_PID_INVALID) return 1;
	init_pid = (process_id_t)parsed_pid;
	return registry_server_run(init_pid, capv[0]);
}
