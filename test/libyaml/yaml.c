#include <criterion/criterion.h>
#include <stdbool.h>
#include <string.h>
#include <yaml.h>

static bool parse_stream(const unsigned char* input, size_t size, size_t* out_aliases, size_t* out_scalars) {
	yaml_parser_t parser;
	yaml_event_t  event;
	bool          done    = false;
	bool          success = true;
	size_t        aliases = 0u;
	size_t        scalars = 0u;

	cr_assert(yaml_parser_initialize(&parser));
	yaml_parser_set_input_string(&parser, input, size);
	while (!done) {
		if (!yaml_parser_parse(&parser, &event)) {
			success = false;
			break;
		}
		if (event.type == YAML_ALIAS_EVENT) aliases++;
		if (event.type == YAML_SCALAR_EVENT) scalars++;
		done = event.type == YAML_STREAM_END_EVENT;
		yaml_event_delete(&event);
	}
	yaml_parser_delete(&parser);
	if (out_aliases != NULL) *out_aliases = aliases;
	if (out_scalars != NULL) *out_scalars = scalars;
	return success;
}

Test(libyaml, parses_utf8_documents_and_aliases) {
	static const unsigned char document[] = "name: périphérique\n"
											"values: &shared [one, two]\n"
											"copy: *shared\n";
	size_t                     aliases    = 0u;
	size_t                     scalars    = 0u;

	cr_assert(parse_stream(document, sizeof(document) - 1u, &aliases, &scalars));
	cr_assert_eq(aliases, 1u);
	cr_assert_eq(scalars, 6u);
}

Test(libyaml, rejects_malformed_yaml_and_invalid_utf8) {
	static const unsigned char malformed[]    = "mapping: [unterminated\n";
	static const unsigned char invalid_utf8[] = {'n', 'a', 'm', 'e', ':', ' ', 0xc0u, 0xafu, '\n'};

	cr_assert_not(parse_stream(malformed, sizeof(malformed) - 1u, NULL, NULL));
	cr_assert_not(parse_stream(invalid_utf8, sizeof(invalid_utf8), NULL, NULL));
}

Test(libyaml, loads_documents_and_resolves_aliases) {
	static const unsigned char document[] = "first: &shared\n"
											"  - one\n"
											"  - two\n"
											"second: *shared\n";
	yaml_parser_t              parser;
	yaml_document_t            loaded;
	yaml_node_t*               root;
	yaml_node_pair_t*          first;
	yaml_node_pair_t*          second;

	cr_assert(yaml_parser_initialize(&parser));
	yaml_parser_set_input_string(&parser, document, sizeof(document) - 1u);
	cr_assert(yaml_parser_load(&parser, &loaded));
	root = yaml_document_get_root_node(&loaded);
	cr_assert_not_null(root);
	cr_assert_eq(root->type, YAML_MAPPING_NODE);
	cr_assert_eq(root->data.mapping.pairs.top - root->data.mapping.pairs.start, 2);
	first  = root->data.mapping.pairs.start;
	second = first + 1;
	cr_assert_eq(first->value, second->value);
	cr_assert_eq(yaml_document_get_node(&loaded, first->value)->type, YAML_SEQUENCE_NODE);
	yaml_document_delete(&loaded);
	yaml_parser_delete(&parser);
}

Test(libyaml, emits_to_a_string_buffer_that_can_be_parsed_again) {
	unsigned char   output[256] = {0};
	size_t          written     = 0u;
	yaml_document_t document;
	yaml_emitter_t  emitter;
	int             mapping;
	int             key;
	int             value;

	cr_assert(yaml_document_initialize(&document, NULL, NULL, NULL, 1, 1));
	mapping = yaml_document_add_mapping(&document, NULL, YAML_BLOCK_MAPPING_STYLE);
	key     = yaml_document_add_scalar(&document, NULL, (const yaml_char_t*)"driver", -1, YAML_PLAIN_SCALAR_STYLE);
	value   = yaml_document_add_scalar(&document, NULL, (const yaml_char_t*)"acpi:tpm2", -1, YAML_PLAIN_SCALAR_STYLE);
	cr_assert_neq(mapping, 0);
	cr_assert_neq(key, 0);
	cr_assert_neq(value, 0);
	cr_assert(yaml_document_append_mapping_pair(&document, mapping, key, value));

	cr_assert(yaml_emitter_initialize(&emitter));
	yaml_emitter_set_output_string(&emitter, output, sizeof(output), &written);
	cr_assert(yaml_emitter_open(&emitter));
	cr_assert(yaml_emitter_dump(&emitter, &document));
	cr_assert(yaml_emitter_close(&emitter));
	yaml_emitter_delete(&emitter);
	cr_assert_gt(written, 0u);
	cr_assert(parse_stream(output, written, NULL, NULL));
}
