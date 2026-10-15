#include <criterion/criterion.h>
#include <mxml.h>
#include <stdlib.h>
#include <string.h>

static void count_error(void* data, const char* message) {
	(void)message;
	(*(unsigned*)data)++;
}

Test(mxml, parses_utf8_attributes_text_and_entities) {
	static const char document[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?><keyboard locale=\"fr\"><key id=\"é\">A &amp; Z</key></keyboard>";
	mxml_options_t* options = mxmlOptionsNew();
	cr_assert_not_null(options);
	mxmlOptionsSetTypeValue(options, MXML_TYPE_OPAQUE);
	mxml_node_t* tree = mxmlLoadString(NULL, options, document);
	cr_assert_not_null(tree);
	mxml_node_t* keyboard = mxmlFindElement(tree, tree, "keyboard", NULL, NULL, MXML_DESCEND_ALL);
	cr_assert_not_null(keyboard);
	cr_assert_str_eq(mxmlElementGetAttr(keyboard, "locale"), "fr");
	mxml_node_t* key = mxmlFindElement(keyboard, keyboard, "key", NULL, NULL, MXML_DESCEND_ALL);
	cr_assert_not_null(key);
	cr_assert_str_eq(mxmlElementGetAttr(key, "id"), "é");
	cr_assert_str_eq(mxmlGetOpaque(mxmlGetFirstChild(key)), "A & Z");
	mxmlDelete(tree);
	mxmlOptionsDelete(options);
}

Test(mxml, saves_and_reloads_a_document) {
	mxml_node_t* document = mxmlNewXML("1.0");
	cr_assert_not_null(document);
	mxml_node_t* root = mxmlNewElement(document, "keyboard");
	cr_assert_not_null(root);
	mxmlElementSetAttr(root, "locale", "fr");
	mxml_node_t* value = mxmlNewReal(root, 123.5);
	cr_assert_not_null(value);
	char* serialized = mxmlSaveAllocString(document, NULL);
	cr_assert_not_null(serialized);
	cr_assert_not_null(strstr(serialized, "123.5"));
	mxml_node_t* loaded = mxmlLoadString(NULL, NULL, serialized);
	cr_assert_not_null(loaded);
	free(serialized);
	mxmlDelete(loaded);
	mxmlDelete(document);
}

Test(mxml, rejects_malformed_xml) {
	unsigned        errors  = 0u;
	mxml_options_t* options = mxmlOptionsNew();
	cr_assert_not_null(options);
	mxmlOptionsSetErrorCallback(options, count_error, &errors);
	cr_assert_null(mxmlLoadString(NULL, options, "<root><child></root>"));
	cr_assert_gt(errors, 0u);
	mxmlOptionsDelete(options);
}
