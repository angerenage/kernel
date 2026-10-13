#include <base/cap.h>
#include <stddef.h>
#include <stdio.h>

#include "input_manager.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct input_manager manager;
	(void)argc;
	(void)argv;
	if (capc != 0u || capv != NULL) {
		printf("input manager: unexpected startup capabilities\n");
		return 1;
	}
	if (!input_manager_initialize(&manager)) {
		printf("input manager: initialization failed\n");
		return 1;
	}
	int result = input_manager_run(&manager);
	printf("input manager: aggregation stopped after an error\n");
	return result;
}
