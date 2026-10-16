#include "layout.h"

#include <mxml.h>
#include <protocol/keyboard.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

#define TEXT_LAYOUT_MAXIMUM_KEYS 512u
#define TEXT_LAYOUT_MAXIMUM_LAYERS 32u
#define TEXT_LAYOUT_MAXIMUM_TRANSFORMS 256u
#define TEXT_LAYOUT_MAXIMUM_VALUE_SIZE 256u

#define LAYOUT_MODIFIER_SHIFT (1u << 0u)
#define LAYOUT_MODIFIER_CAPS (1u << 1u)
#define LAYOUT_MODIFIER_ALT_LEFT (1u << 2u)
#define LAYOUT_MODIFIER_ALT_RIGHT (1u << 3u)
#define LAYOUT_MODIFIER_CONTROL_LEFT (1u << 4u)
#define LAYOUT_MODIFIER_CONTROL_RIGHT (1u << 5u)

struct text_layout_key {
	struct text_layout_key* next;
	char*                   id;
	char*                   output;
};

struct text_layout_transform {
	struct text_layout_transform* next;
	char*                         from;
	size_t                        from_size;
	char*                         to;
	size_t                        to_size;
};

struct form_description {
	const enum input_key_code* rows[4];
	size_t                     row_sizes[4];
};

static const enum input_key_code number_row[] = {
	INPUT_KEY_GRAVE,
	INPUT_KEY_1,
	INPUT_KEY_2,
	INPUT_KEY_3,
	INPUT_KEY_4,
	INPUT_KEY_5,
	INPUT_KEY_6,
	INPUT_KEY_7,
	INPUT_KEY_8,
	INPUT_KEY_9,
	INPUT_KEY_0,
	INPUT_KEY_MINUS,
	INPUT_KEY_EQUAL,
};

static const enum input_key_code upper_row_us[] = {
	INPUT_KEY_Q,
	INPUT_KEY_W,
	INPUT_KEY_E,
	INPUT_KEY_R,
	INPUT_KEY_T,
	INPUT_KEY_Y,
	INPUT_KEY_U,
	INPUT_KEY_I,
	INPUT_KEY_O,
	INPUT_KEY_P,
	INPUT_KEY_LEFT_BRACKET,
	INPUT_KEY_RIGHT_BRACKET,
	INPUT_KEY_BACKSLASH,
};

static const enum input_key_code upper_row_iso[] = {
	INPUT_KEY_Q,
	INPUT_KEY_W,
	INPUT_KEY_E,
	INPUT_KEY_R,
	INPUT_KEY_T,
	INPUT_KEY_Y,
	INPUT_KEY_U,
	INPUT_KEY_I,
	INPUT_KEY_O,
	INPUT_KEY_P,
	INPUT_KEY_LEFT_BRACKET,
	INPUT_KEY_RIGHT_BRACKET,
};

static const enum input_key_code home_row_us[] = {
	INPUT_KEY_A,
	INPUT_KEY_S,
	INPUT_KEY_D,
	INPUT_KEY_F,
	INPUT_KEY_G,
	INPUT_KEY_H,
	INPUT_KEY_J,
	INPUT_KEY_K,
	INPUT_KEY_L,
	INPUT_KEY_SEMICOLON,
	INPUT_KEY_APOSTROPHE,
};

static const enum input_key_code home_row_iso[] = {
	INPUT_KEY_A,
	INPUT_KEY_S,
	INPUT_KEY_D,
	INPUT_KEY_F,
	INPUT_KEY_G,
	INPUT_KEY_H,
	INPUT_KEY_J,
	INPUT_KEY_K,
	INPUT_KEY_L,
	INPUT_KEY_SEMICOLON,
	INPUT_KEY_APOSTROPHE,
	INPUT_KEY_BACKSLASH,
};

static const enum input_key_code lower_row_us[] = {
	INPUT_KEY_Z,
	INPUT_KEY_X,
	INPUT_KEY_C,
	INPUT_KEY_V,
	INPUT_KEY_B,
	INPUT_KEY_N,
	INPUT_KEY_M,
	INPUT_KEY_COMMA,
	INPUT_KEY_PERIOD,
	INPUT_KEY_SLASH,
};

static const enum input_key_code lower_row_iso[] = {
	INPUT_KEY_NON_US_BACKSLASH,
	INPUT_KEY_Z,
	INPUT_KEY_X,
	INPUT_KEY_C,
	INPUT_KEY_V,
	INPUT_KEY_B,
	INPUT_KEY_N,
	INPUT_KEY_M,
	INPUT_KEY_COMMA,
	INPUT_KEY_PERIOD,
	INPUT_KEY_SLASH,
};

static void layout_error(void* data, const char* message) {
	(void)data;
	printf("text input: LDML parse error: %s\n", message);
}

static bool is_element(const mxml_node_t* node, const char* name) {
	return node != NULL && mxmlGetType((mxml_node_t*)node) == MXML_TYPE_ELEMENT &&
	       strcmp(mxmlGetElement((mxml_node_t*)node), name) == 0;
}

static mxml_node_t* first_element_child(mxml_node_t* parent) {
	for (mxml_node_t* child = mxmlGetFirstChild(parent); child != NULL; child = mxmlGetNextSibling(child))
		if (mxmlGetType(child) == MXML_TYPE_ELEMENT) return child;
	return NULL;
}

static mxml_node_t* next_element_sibling(mxml_node_t* node) {
	for (node = mxmlGetNextSibling(node); node != NULL; node = mxmlGetNextSibling(node))
		if (mxmlGetType(node) == MXML_TYPE_ELEMENT) return node;
	return NULL;
}

static bool utf8_valid(const char* string) {
	if (string == NULL) return false;
	size_t size   = strlen(string);
	size_t offset = 0u;
	while (offset < size) {
		utf8proc_int32_t codepoint;
		utf8proc_ssize_t consumed =
			utf8proc_iterate((const utf8proc_uint8_t*)string + offset, (utf8proc_ssize_t)(size - offset), &codepoint);
		if (consumed <= 0 || codepoint == 0) return false;
		offset += (size_t)consumed;
	}
	return true;
}

static bool append_bytes(char* output, size_t* size, const void* bytes, size_t count) {
	if (count > TEXT_LAYOUT_MAXIMUM_VALUE_SIZE - 1u - *size) return false;
	memcpy(output + *size, bytes, count);
	*size += count;
	return true;
}

static bool parse_hex_scalar(const char** cursor, uint32_t* scalar) {
	uint32_t value  = 0u;
	size_t   digits = 0u;
	while (**cursor != '\0' && **cursor != '}') {
		unsigned digit;
		if (**cursor >= '0' && **cursor <= '9') digit = (unsigned)(**cursor - '0');
		else if (**cursor >= 'a' && **cursor <= 'f') digit = (unsigned)(**cursor - 'a') + 10u;
		else if (**cursor >= 'A' && **cursor <= 'F') digit = (unsigned)(**cursor - 'A') + 10u;
		else return false;
		if (digits == 6u || value > (0x10ffffu - digit) / 16u) return false;
		value = value * 16u + digit;
		digits++;
		(*cursor)++;
	}
	if (**cursor != '}' || digits == 0u || value == 0u || (value >= 0xd800u && value <= 0xdfffu)) return false;
	(*cursor)++;
	*scalar = value;
	return true;
}

static char* decode_value(const char* value, bool pattern) {
	char   decoded[TEXT_LAYOUT_MAXIMUM_VALUE_SIZE];
	size_t size = 0u;
	if (value == NULL || !utf8_valid(value)) return NULL;
	for (const char* cursor = value; *cursor != '\0';) {
		if (*cursor != '\\') {
			if (pattern && strchr(".()?[{}*+|$^", *cursor) != NULL) return NULL;
			const char* start = cursor++;
			while ((*cursor & 0xc0) == 0x80) cursor++;
			if (!append_bytes(decoded, &size, start, (size_t)(cursor - start))) return NULL;
			continue;
		}
		cursor++;
		if (cursor[0] == 'u' && cursor[1] == '{') {
			uint32_t scalar;
			cursor += 2;
			if (!parse_hex_scalar(&cursor, &scalar)) return NULL;
			utf8proc_uint8_t encoded[4];
			utf8proc_ssize_t encoded_size = utf8proc_encode_char((utf8proc_int32_t)scalar, encoded);
			if (encoded_size <= 0 || !append_bytes(decoded, &size, encoded, (size_t)encoded_size)) return NULL;
			continue;
		}
		if (!pattern || *cursor == '\0' || strchr(".()?[\\]{}*/^+|$", *cursor) == NULL) return NULL;
		if (!append_bytes(decoded, &size, cursor, 1u)) return NULL;
		cursor++;
	}
	decoded[size]                     = '\0';
	utf8proc_uint8_t* normalized      = NULL;
	utf8proc_ssize_t  normalized_size = utf8proc_map(
		(const utf8proc_uint8_t*)decoded, (utf8proc_ssize_t)size, &normalized, UTF8PROC_STABLE | UTF8PROC_DECOMPOSE);
	if (normalized_size < 0 || normalized == NULL || normalized_size > TEXT_LAYOUT_MAXIMUM_VALUE_SIZE - 1u) {
		if (normalized != NULL) utf8proc_free(normalized);
		return NULL;
	}
	return (char*)normalized;
}

static struct text_layout_key* find_key(const struct text_layout* layout, const char* id) {
	for (struct text_layout_key* key = layout->keys; key != NULL; key = key->next)
		if (strcmp(key->id, id) == 0) return key;
	return NULL;
}

static bool parse_keys(struct text_layout* layout, mxml_node_t* keys) {
	size_t count = 0u;
	for (mxml_node_t* node = first_element_child(keys); node != NULL; node = next_element_sibling(node)) {
		if (!is_element(node, "key") || ++count > TEXT_LAYOUT_MAXIMUM_KEYS) return false;
		const char* id     = mxmlElementGetAttr(node, "id");
		const char* output = mxmlElementGetAttr(node, "output");
		const char* gap    = mxmlElementGetAttr(node, "gap");
		bool        is_gap = gap != NULL && strcmp(gap, "true") == 0;
		if (id == NULL || id[0] == '\0' || strlen(id) > 64u || find_key(layout, id) != NULL ||
		    (gap != NULL && !is_gap) || (output == NULL && !is_gap) ||
		    (output != NULL && (output[0] == '\0' || is_gap)))
			return false;
		struct text_layout_key* key = calloc(1u, sizeof(*key));
		if (key == NULL) return false;
		key->id = strdup(id);
		if (output != NULL) key->output = decode_value(output, false);
		if (key->id == NULL ||
		    (output != NULL && (key->output == NULL || strlen(key->output) > TEXT_LAYOUT_CONTEXT_CAPACITY))) {
			free(key->id);
			free(key->output);
			free(key);
			return false;
		}
		key->next    = layout->keys;
		layout->keys = key;
	}
	return count != 0u;
}

static bool form_description(const char* id, struct form_description* form) {
	if (id == NULL || form == NULL || (strcmp(id, "us") != 0 && strcmp(id, "iso") != 0)) return false;
	bool iso           = strcmp(id, "iso") == 0;
	form->rows[0]      = number_row;
	form->rows[1]      = iso ? upper_row_iso : upper_row_us;
	form->rows[2]      = iso ? home_row_iso : home_row_us;
	form->rows[3]      = iso ? lower_row_iso : lower_row_us;
	form->row_sizes[0] = sizeof(number_row) / sizeof(number_row[0]);
	form->row_sizes[1] =
		iso ? sizeof(upper_row_iso) / sizeof(upper_row_iso[0]) : sizeof(upper_row_us) / sizeof(upper_row_us[0]);
	form->row_sizes[2] =
		iso ? sizeof(home_row_iso) / sizeof(home_row_iso[0]) : sizeof(home_row_us) / sizeof(home_row_us[0]);
	form->row_sizes[3] =
		iso ? sizeof(lower_row_iso) / sizeof(lower_row_iso[0]) : sizeof(lower_row_us) / sizeof(lower_row_us[0]);
	return true;
}

static bool component_matches(const char* component, uint8_t state) {
	if (strcmp(component, "shift") == 0) return (state & LAYOUT_MODIFIER_SHIFT) != 0u;
	if (strcmp(component, "caps") == 0) return (state & LAYOUT_MODIFIER_CAPS) != 0u;
	if (strcmp(component, "alt") == 0) return (state & (LAYOUT_MODIFIER_ALT_LEFT | LAYOUT_MODIFIER_ALT_RIGHT)) != 0u;
	if (strcmp(component, "altL") == 0) return (state & LAYOUT_MODIFIER_ALT_LEFT) != 0u;
	if (strcmp(component, "altR") == 0) return (state & LAYOUT_MODIFIER_ALT_RIGHT) != 0u;
	if (strcmp(component, "ctrl") == 0)
		return (state & (LAYOUT_MODIFIER_CONTROL_LEFT | LAYOUT_MODIFIER_CONTROL_RIGHT)) != 0u;
	if (strcmp(component, "ctrlL") == 0) return (state & LAYOUT_MODIFIER_CONTROL_LEFT) != 0u;
	if (strcmp(component, "ctrlR") == 0) return (state & LAYOUT_MODIFIER_CONTROL_RIGHT) != 0u;
	return false;
}

static char* next_token(char** cursor, char separator) {
	char* token;
	while (**cursor == ' ') (*cursor)++;
	if (**cursor == '\0') return NULL;
	token = *cursor;
	while (**cursor != '\0' && **cursor != separator && **cursor != ' ') (*cursor)++;
	if (**cursor != '\0') *(*cursor)++ = '\0';
	return token;
}

static bool modifier_set_matches(char* set, uint8_t state, bool* valid) {
	uint8_t mentioned = 0u;
	bool    any       = false;
	*valid            = true;
	while (*set == ' ') set++;
	if (strcmp(set, "none") == 0) return state == 0u;
	if (strcmp(set, "other") == 0) {
		*valid = false;
		return false;
	}
	char* cursor = set;
	for (char* component = next_token(&cursor, ','); component != NULL; component = next_token(&cursor, ',')) {
		uint8_t mask;
		if (strcmp(component, "shift") == 0) mask = LAYOUT_MODIFIER_SHIFT;
		else if (strcmp(component, "caps") == 0) mask = LAYOUT_MODIFIER_CAPS;
		else if (strcmp(component, "alt") == 0) mask = LAYOUT_MODIFIER_ALT_LEFT | LAYOUT_MODIFIER_ALT_RIGHT;
		else if (strcmp(component, "altL") == 0) mask = LAYOUT_MODIFIER_ALT_LEFT;
		else if (strcmp(component, "altR") == 0) mask = LAYOUT_MODIFIER_ALT_RIGHT;
		else if (strcmp(component, "ctrl") == 0) mask = LAYOUT_MODIFIER_CONTROL_LEFT | LAYOUT_MODIFIER_CONTROL_RIGHT;
		else if (strcmp(component, "ctrlL") == 0) mask = LAYOUT_MODIFIER_CONTROL_LEFT;
		else if (strcmp(component, "ctrlR") == 0) mask = LAYOUT_MODIFIER_CONTROL_RIGHT;
		else {
			*valid = false;
			return false;
		}
		if ((mentioned & mask) != 0u || !component_matches(component, state)) return false;
		mentioned |= mask;
		any = true;
	}
	return any && (state & ~mentioned) == 0u;
}

static bool layer_states(const char* modifiers, bool states[64]) {
	char* copy = modifiers == NULL ? NULL : strdup(modifiers);
	if (copy == NULL || copy[0] == '\0') {
		free(copy);
		return false;
	}
	memset(states, 0, 64u * sizeof(states[0]));
	for (char* set = copy; set != NULL;) {
		char* comma = strchr(set, ',');
		if (comma != NULL) *comma = '\0';
		for (uint8_t state = 0u; state < 64u; state++) {
			char* state_set = strdup(set);
			bool  valid;
			bool  matches = state_set != NULL && modifier_set_matches(state_set, state, &valid);
			free(state_set);
			if (state_set == NULL || !valid) {
				free(copy);
				return false;
			}
			if (matches) states[state] = true;
		}
		set = comma == NULL ? NULL : comma + 1u;
	}
	free(copy);
	for (size_t state = 0u; state < 64u; state++)
		if (states[state]) return true;
	return false;
}

static bool map_row(struct text_layout* layout, const bool states[64], const struct form_description* form,
                    size_t row_index, const char* key_ids) {
	char* copy = key_ids == NULL ? NULL : strdup(key_ids);
	if (copy == NULL) return false;
	size_t column = 0u;
	char*  cursor = copy;
	for (char* id = next_token(&cursor, ','); id != NULL; id = next_token(&cursor, ',')) {
		if (column >= form->row_sizes[row_index]) {
			free(copy);
			return false;
		}
		struct text_layout_key* key = find_key(layout, id);
		if (key == NULL) {
			free(copy);
			return false;
		}
		for (size_t state = 0u; state < 64u; state++) {
			if (!states[state]) continue;
			enum input_key_code physical = form->rows[row_index][column];
			if (layout->mappings[state][physical] != NULL) {
				free(copy);
				return false;
			}
			layout->mappings[state][physical] = key->output;
		}
		column++;
	}
	free(copy);
	return column == form->row_sizes[row_index];
}

static bool parse_layers(struct text_layout* layout, mxml_node_t* layers) {
	struct form_description form;
	if (!form_description(mxmlElementGetAttr(layers, "formId"), &form)) return false;
	size_t layer_count  = 0u;
	bool   assigned[64] = {false};
	for (mxml_node_t* layer = first_element_child(layers); layer != NULL; layer = next_element_sibling(layer)) {
		if (!is_element(layer, "layer") || ++layer_count > TEXT_LAYOUT_MAXIMUM_LAYERS) return false;
		bool states[64];
		if (!layer_states(mxmlElementGetAttr(layer, "modifiers"), states)) return false;
		for (size_t state = 0u; state < 64u; state++) {
			if (states[state] && assigned[state]) return false;
			if (states[state]) {
				assigned[state]              = true;
				layout->active_states[state] = true;
			}
		}
		size_t row_index = 0u;
		for (mxml_node_t* row = first_element_child(layer); row != NULL; row = next_element_sibling(row)) {
			if (!is_element(row, "row") || row_index >= 4u ||
			    !map_row(layout, states, &form, row_index, mxmlElementGetAttr(row, "keys")))
				return false;
			row_index++;
		}
		if (row_index != 4u) return false;
	}
	return layer_count != 0u;
}

static bool parse_transforms(struct text_layout* layout, mxml_node_t* transforms) {
	const char* type = mxmlElementGetAttr(transforms, "type");
	if (type == NULL || strcmp(type, "simple") != 0) return false;
	size_t count = 0u;
	for (mxml_node_t* group = first_element_child(transforms); group != NULL; group = next_element_sibling(group)) {
		if (!is_element(group, "transformGroup")) return false;
		for (mxml_node_t* node = first_element_child(group); node != NULL; node = next_element_sibling(node)) {
			if (!is_element(node, "transform") || ++count > TEXT_LAYOUT_MAXIMUM_TRANSFORMS) return false;
			const char*                   from      = mxmlElementGetAttr(node, "from");
			const char*                   to        = mxmlElementGetAttr(node, "to");
			struct text_layout_transform* transform = calloc(1u, sizeof(*transform));
			if (transform == NULL) return false;
			transform->from = decode_value(from, true);
			transform->to   = decode_value(to == NULL ? "" : to, false);
			if (transform->from == NULL || transform->from[0] == '\0' || transform->to == NULL ||
			    transform->to[0] == '\0' || strlen(transform->from) > TEXT_LAYOUT_CONTEXT_CAPACITY ||
			    strlen(transform->to) > TEXT_LAYOUT_CONTEXT_CAPACITY) {
				free(transform->from);
				free(transform->to);
				free(transform);
				return false;
			}
			transform->from_size                = strlen(transform->from);
			transform->to_size                  = strlen(transform->to);
			transform->next                     = NULL;
			struct text_layout_transform** tail = &layout->transforms;
			while (*tail != NULL) tail = &(*tail)->next;
			*tail = transform;
		}
	}
	return count != 0u;
}

static bool parse_document(struct text_layout* layout, mxml_node_t* tree) {
	mxml_node_t* root = mxmlFindElement(tree, tree, "keyboard3", NULL, NULL, MXML_DESCEND_ALL);
	if (root == NULL || mxmlElementGetAttr(root, "locale") == NULL || mxmlElementGetAttr(root, "conformsTo") == NULL)
		return false;
	const char* version = mxmlElementGetAttr(root, "conformsTo");
	char*       version_end;
	long        version_number = strtol(version, &version_end, 10);
	if (*version == '\0' || *version_end != '\0' || version_number < 45 || version_number > 48 ||
	    !utf8_valid(mxmlElementGetAttr(root, "locale")) || mxmlElementGetAttr(root, "locale")[0] == '\0')
		return false;
	layout->locale = strdup(mxmlElementGetAttr(root, "locale"));
	if (layout->locale == NULL) return false;
	mxml_node_t* keys                   = NULL;
	mxml_node_t* layers                 = NULL;
	bool         simple_transforms_seen = false;
	for (mxml_node_t* node = first_element_child(root); node != NULL; node = next_element_sibling(node)) {
		if (is_element(node, "keys")) {
			if (keys != NULL) return false;
			keys = node;
		}
		else if (is_element(node, "layers")) {
			if (layers != NULL) return false;
			layers = node;
		}
		else if (is_element(node, "transforms")) {
			if (simple_transforms_seen) return false;
			if (!parse_transforms(layout, node)) return false;
			simple_transforms_seen = true;
		}
		else if (!is_element(node, "info") && !is_element(node, "version") && !is_element(node, "displays")) {
			return false;
		}
	}
	return keys != NULL && layers != NULL && parse_keys(layout, keys) && parse_layers(layout, layers);
}

bool text_layout_load(struct text_layout* layout, const char* path) {
	if (layout == NULL || path == NULL || path[0] != '/') return false;
	memset(layout, 0, sizeof(*layout));
	mxml_options_t* options = mxmlOptionsNew();
	if (options == NULL) return false;
	mxmlOptionsSetErrorCallback(options, layout_error, NULL);
	mxml_node_t* tree = mxmlLoadFilename(NULL, options, path);
	mxmlOptionsDelete(options);
	if (tree == NULL) return false;
	bool success = parse_document(layout, tree);
	mxmlDelete(tree);
	if (!success) text_layout_destroy(layout);
	return success;
}

void text_layout_destroy(struct text_layout* layout) {
	if (layout == NULL) return;
	while (layout->keys != NULL) {
		struct text_layout_key* key = layout->keys;
		layout->keys                = key->next;
		free(key->id);
		free(key->output);
		free(key);
	}
	while (layout->transforms != NULL) {
		struct text_layout_transform* transform = layout->transforms;
		layout->transforms                      = transform->next;
		free(transform->from);
		free(transform->to);
		free(transform);
	}
	free(layout->locale);
	memset(layout, 0, sizeof(*layout));
}

static uint8_t modifier_state(uint32_t modifiers, uint32_t locks) {
	uint8_t state = 0u;
	if ((modifiers & (KEYBOARD_MODIFIER_LEFT_SHIFT | KEYBOARD_MODIFIER_RIGHT_SHIFT)) != 0u)
		state |= LAYOUT_MODIFIER_SHIFT;
	if ((locks & KEYBOARD_LOCK_CAPS_LOCK) != 0u) state |= LAYOUT_MODIFIER_CAPS;
	if ((modifiers & KEYBOARD_MODIFIER_LEFT_ALT) != 0u) state |= LAYOUT_MODIFIER_ALT_LEFT;
	if ((modifiers & KEYBOARD_MODIFIER_RIGHT_ALT) != 0u) state |= LAYOUT_MODIFIER_ALT_RIGHT;
	if ((modifiers & KEYBOARD_MODIFIER_LEFT_CONTROL) != 0u) state |= LAYOUT_MODIFIER_CONTROL_LEFT;
	if ((modifiers & KEYBOARD_MODIFIER_RIGHT_CONTROL) != 0u) state |= LAYOUT_MODIFIER_CONTROL_RIGHT;
	return state;
}

const char* text_layout_map(const struct text_layout* layout, enum input_key_code key, uint32_t modifiers,
                            uint32_t locks) {
	if (layout == NULL || key <= INPUT_KEY_INVALID || key > INPUT_KEY_KEYPAD_DECIMAL ||
	    (modifiers & (KEYBOARD_MODIFIER_LEFT_META | KEYBOARD_MODIFIER_RIGHT_META)) != 0u)
		return NULL;
	uint8_t state = modifier_state(modifiers, locks);
	if (!layout->active_states[state]) return NULL;
	/*
	 * LDML excludes frame keys such as the optional numeric keypad, so their
	 * conventional output is platform-defined.
	 */
	if (key == INPUT_KEY_SPACE) return " ";
	if (key == INPUT_KEY_TAB) return "\t";
	if (key == INPUT_KEY_ENTER || key == INPUT_KEY_KEYPAD_ENTER) return "\n";
	if (key == INPUT_KEY_KEYPAD_DIVIDE) return "/";
	if (key == INPUT_KEY_KEYPAD_MULTIPLY) return "*";
	if (key == INPUT_KEY_KEYPAD_SUBTRACT) return "-";
	if (key == INPUT_KEY_KEYPAD_ADD) return "+";
	if ((locks & KEYBOARD_LOCK_NUM_LOCK) != 0u) {
		switch (key) {
		case INPUT_KEY_KEYPAD_0:
			return "0";
		case INPUT_KEY_KEYPAD_1:
			return "1";
		case INPUT_KEY_KEYPAD_2:
			return "2";
		case INPUT_KEY_KEYPAD_3:
			return "3";
		case INPUT_KEY_KEYPAD_4:
			return "4";
		case INPUT_KEY_KEYPAD_5:
			return "5";
		case INPUT_KEY_KEYPAD_6:
			return "6";
		case INPUT_KEY_KEYPAD_7:
			return "7";
		case INPUT_KEY_KEYPAD_8:
			return "8";
		case INPUT_KEY_KEYPAD_9:
			return "9";
		default:
			break;
		}
	}
	if (key == INPUT_KEY_KEYPAD_DECIMAL && (locks & KEYBOARD_LOCK_NUM_LOCK) != 0u)
		return layout->decimal_comma ? "," : ".";
	return layout->mappings[state][key];
}

const char* text_layout_shortcut_map(const struct text_layout* layout, enum input_key_code key, uint32_t modifiers,
                                     uint32_t locks) {
	const uint32_t shortcut_modifiers = KEYBOARD_MODIFIER_LEFT_CONTROL | KEYBOARD_MODIFIER_RIGHT_CONTROL |
	                                    KEYBOARD_MODIFIER_LEFT_ALT | KEYBOARD_MODIFIER_LEFT_META |
	                                    KEYBOARD_MODIFIER_RIGHT_META;
	return text_layout_map(layout, key, modifiers & ~shortcut_modifiers, locks);
}

enum text_layout_result text_layout_apply(const struct text_layout* layout, const uint8_t* context, size_t context_size,
                                          const char* output, uint8_t* next_context, size_t* next_context_size,
                                          uint8_t** committed, size_t* committed_size) {
	if (next_context_size != NULL) *next_context_size = 0u;
	if (committed != NULL) *committed = NULL;
	if (committed_size != NULL) *committed_size = 0u;
	if (layout == NULL || (context_size != 0u && context == NULL) || output == NULL || next_context == NULL ||
	    next_context_size == NULL || committed == NULL || committed_size == NULL)
		return TEXT_LAYOUT_RESULT_NONE;
	size_t output_size = strlen(output);
	if (output_size > TEXT_LAYOUT_CONTEXT_CAPACITY - context_size) return TEXT_LAYOUT_RESULT_NONE;
	memcpy(next_context, context, context_size);
	memcpy(next_context + context_size, output, output_size);
	*next_context_size = context_size + output_size;
	bool prefix        = false;
	for (const struct text_layout_transform* transform = layout->transforms; transform != NULL;
	     transform                                     = transform->next) {
		if (transform->from_size == *next_context_size &&
		    memcmp(transform->from, next_context, *next_context_size) == 0) {
			utf8proc_uint8_t* normalized = NULL;
			utf8proc_ssize_t  size       = utf8proc_map((const utf8proc_uint8_t*)transform->to,
			                                            (utf8proc_ssize_t)transform->to_size,
			                                            &normalized,
			                                            UTF8PROC_STABLE | UTF8PROC_COMPOSE);
			if (size < 0 || normalized == NULL) return TEXT_LAYOUT_RESULT_NONE;
			*committed         = normalized;
			*committed_size    = (size_t)size;
			*next_context_size = 0u;
			return TEXT_LAYOUT_RESULT_COMMIT;
		}
		if (transform->from_size > *next_context_size && memcmp(transform->from, next_context, *next_context_size) == 0)
			prefix = true;
	}
	if (prefix) return TEXT_LAYOUT_RESULT_PENDING;
	utf8proc_uint8_t* normalized = NULL;
	utf8proc_ssize_t  size       = utf8proc_map(
		next_context, (utf8proc_ssize_t)*next_context_size, &normalized, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	if (size < 0 || normalized == NULL) return TEXT_LAYOUT_RESULT_NONE;
	*committed         = normalized;
	*committed_size    = (size_t)size;
	*next_context_size = 0u;
	return TEXT_LAYOUT_RESULT_COMMIT;
}
