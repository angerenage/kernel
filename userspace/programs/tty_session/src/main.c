#include <base/cap.h>
#include <stddef.h>
#include <stdio.h>

#include "session.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argv;
	if (argc != 0 || capc != 0u || capv != NULL) {
		printf("tty session: invalid startup arguments\n");
		return 1;
	}
	return tty_session_run();
}
