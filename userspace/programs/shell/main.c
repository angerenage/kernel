#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define SHELL_LINE_CAPACITY 4096u

enum input_state {
	INPUT_STATE_TEXT,
	INPUT_STATE_ESCAPE,
	INPUT_STATE_CSI,
	INPUT_STATE_SS3,
};

static bool write_all(const void* data, size_t size) {
	const uint8_t* bytes  = data;
	size_t         offset = 0u;

	while (offset < size) {
		ssize_t written = write(1, bytes + offset, size - offset);
		if (written <= 0 || (size_t)written > size - offset) return false;
		offset += (size_t)written;
	}
	return true;
}

#define WRITE_LITERAL(value) write_all((value), sizeof(value) - 1u)

static bool write_prompt(void) {
	return WRITE_LITERAL("\x1b[1;34mkernel\x1b[0m:\x1b[1;36m/\x1b[0m\x1b[1;32m$\x1b[0m ");
}

static bool write_result(const uint8_t* line, size_t size) {
	return WRITE_LITERAL("\x1b[1;35mYou entered:\x1b[0m ") && write_all(line, size) && WRITE_LITERAL("\n");
}

static void erase_last_codepoint(uint8_t* line, size_t* size) {
	if (*size == 0u) return;
	(*size)--;
	while (*size != 0u && (line[*size] & 0xc0u) == 0x80u) (*size)--;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	uint8_t          line[SHELL_LINE_CAPACITY];
	size_t           line_size = 0u;
	enum input_state state     = INPUT_STATE_TEXT;

	(void)argv;
	if (argc != 0 || capc != 0u || capv != NULL) return 1;
	if (!WRITE_LITERAL("\x1b[1;37mBasic shell\x1b[0m\n") || !write_prompt()) return 1;
	for (;;) {
		uint8_t byte;
		ssize_t count = read(0, &byte, 1u);
		if (count != 1) return 1;

		if (state == INPUT_STATE_ESCAPE) {
			if (byte == '[') state = INPUT_STATE_CSI;
			else if (byte == 'O') state = INPUT_STATE_SS3;
			else state = INPUT_STATE_TEXT;
			continue;
		}
		if (state == INPUT_STATE_CSI) {
			if (byte >= 0x40u && byte <= 0x7eu) state = INPUT_STATE_TEXT;
			continue;
		}
		if (state == INPUT_STATE_SS3) {
			state = INPUT_STATE_TEXT;
			continue;
		}
		if (byte == 0x1bu) {
			state = INPUT_STATE_ESCAPE;
			continue;
		}
		if (byte == '\n' || byte == '\r') {
			if (!WRITE_LITERAL("\n") || !write_result(line, line_size) || !write_prompt()) return 1;
			line_size = 0u;
			continue;
		}
		if (byte == 0x03u) {
			if (!WRITE_LITERAL("^C\n") || !write_prompt()) return 1;
			line_size = 0u;
			continue;
		}
		if (byte == '\b' || byte == 0x7fu) {
			if (line_size != 0u) {
				erase_last_codepoint(line, &line_size);
				if (!WRITE_LITERAL("\b \b")) return 1;
			}
			continue;
		}
		if (byte < 0x20u || line_size == sizeof(line)) continue;
		line[line_size++] = byte;
		if (!write_all(&byte, 1u)) return 1;
	}
}
