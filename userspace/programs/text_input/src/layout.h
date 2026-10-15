#pragma once

#include <protocol/input.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEXT_LAYOUT_CONTEXT_CAPACITY 64u

struct text_layout_key;
struct text_layout_transform;

/* One immutable keyboard layout compiled from an LDML Keyboard 3 document. */
struct text_layout {
	const char*                   mappings[64u][INPUT_KEY_KEYPAD_DECIMAL + 1u];
	struct text_layout_key*       keys;
	struct text_layout_transform* transforms;
	char*                         locale;
	bool                          active_states[64u];
	bool                          decimal_comma;
};

/* Result of applying one key output to the current transform context. */
enum text_layout_result {
	TEXT_LAYOUT_RESULT_NONE,
	TEXT_LAYOUT_RESULT_PENDING,
	TEXT_LAYOUT_RESULT_COMMIT,
};

/* Load and validate the supported hardware subset of an LDML Keyboard 3 file. */
bool text_layout_load(struct text_layout* layout, const char* path);

/* Release all storage owned by a loaded layout. */
void text_layout_destroy(struct text_layout* layout);

/* Resolve one physical key using the event's exact LDML modifier state. */
const char* text_layout_map(const struct text_layout* layout, enum input_key_code key, uint32_t modifiers,
                            uint32_t locks);

/* Apply literal simple transforms to one mapped output in normalized form. */
enum text_layout_result text_layout_apply(const struct text_layout* layout, const uint8_t* context, size_t context_size,
                                          const char* output, uint8_t* next_context, size_t* next_context_size,
                                          uint8_t** committed, size_t* committed_size);
