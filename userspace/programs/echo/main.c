#include <base/cap.h>
#include <stddef.h>
#include <stdio.h>

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	if (argc < 0 || (argc != 0 && argv == NULL) || capc != 0u || capv != NULL) return 1;
	for (int i = 0; i < argc; i++) {
		if (i != 0 && printf(" ") < 0) return 1;
		if (printf("%s", argv[i]) < 0) return 1;
	}
	return printf("\n") < 0 ? 1 : 0;
}
