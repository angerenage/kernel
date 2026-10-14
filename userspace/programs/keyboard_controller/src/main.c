#include <base/cap.h>
#include <stddef.h>
#include <stdio.h>

#include "keyboard_controller.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	static struct keyboard_controller controller;
	(void)argc;
	(void)argv;
	if (capc != 0u || capv != NULL) {
		printf("keyboard controller: unexpected startup capabilities\n");
		return 1;
	}
	if (!keyboard_controller_initialize(&controller)) {
		printf("keyboard controller: initialization failed\n");
		return 1;
	}
	int result = keyboard_controller_run(&controller);
	printf("keyboard controller: aggregation stopped after an error\n");
	return result;
}
