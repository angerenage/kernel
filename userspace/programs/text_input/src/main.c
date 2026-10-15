#include <base/cap.h>
#include <stddef.h>
#include <stdio.h>

#include "text_input.h"

#define TEXT_INPUT_DEFAULT_LAYOUT_PATH "/boot/keyboard_qwerty.xml"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	static struct text_input input;
	if (capc != 0u || capv != NULL) {
		printf("text input: unexpected startup capabilities\n");
		return 1;
	}
	if (argc < 0 || argc > 1 || (argc == 1 && (argv == NULL || argv[0] == NULL || argv[0][0] != '/'))) {
		printf("text input: expected at most one absolute LDML layout path\n");
		return 1;
	}
	const char* layout_path = argc == 0 ? TEXT_INPUT_DEFAULT_LAYOUT_PATH : argv[0];
	if (!text_input_initialize(&input, layout_path)) {
		printf("text input: initialization failed\n");
		return 1;
	}
	int result = text_input_run(&input);
	printf("text input: translation stopped after an error\n");
	return result;
}
