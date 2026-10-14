#include <criterion/criterion.h>
#include <string.h>
#include <utf8proc.h>

Test(utf8proc, reports_pinned_versions) {
	cr_assert_str_eq(utf8proc_version(), "2.12.0");
	cr_assert_str_eq(utf8proc_unicode_version(), "18.0.0");
}

Test(utf8proc, normalizes_canonical_forms) {
	static const utf8proc_uint8_t decomposed[] = {'C', 'a', 'f', 'e', 0xccu, 0x81u, 0};
	static const utf8proc_uint8_t composed[]   = {'C', 'a', 'f', 0xc3u, 0xa9u, 0};
	utf8proc_uint8_t*             nfc;
	utf8proc_uint8_t*             nfd;

	nfc = utf8proc_NFC(decomposed);
	cr_assert_not_null(nfc);
	cr_assert_arr_eq(nfc, composed, sizeof(composed));

	nfd = utf8proc_NFD(composed);
	cr_assert_not_null(nfd);
	cr_assert_arr_eq(nfd, decomposed, sizeof(decomposed));

	utf8proc_free(nfd);
	utf8proc_free(nfc);
}

Test(utf8proc, normalizes_compatibility_forms) {
	static const utf8proc_uint8_t ligature[] = {0xefu, 0xacu, 0x83u, 0};
	static const utf8proc_uint8_t expanded[] = {'f', 'f', 'i', 0};
	utf8proc_uint8_t*             normalized;

	normalized = utf8proc_NFKC(ligature);
	cr_assert_not_null(normalized);
	cr_assert_arr_eq(normalized, expanded, sizeof(expanded));
	utf8proc_free(normalized);
}

Test(utf8proc, rejects_invalid_utf8) {
	static const utf8proc_uint8_t invalid[] = {0xc0u, 0xafu};
	utf8proc_uint8_t*             output    = NULL;
	utf8proc_ssize_t              result;

	result = utf8proc_map(invalid, sizeof(invalid), &output, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	cr_assert_eq(result, UTF8PROC_ERROR_INVALIDUTF8);
	cr_assert_null(output);
}

Test(utf8proc, preserves_embedded_nul_with_explicit_lengths) {
	static const utf8proc_uint8_t input[] = {'A', 0, 'B'};
	utf8proc_uint8_t*             output  = NULL;
	utf8proc_ssize_t              result;

	result = utf8proc_map(input, sizeof(input), &output, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
	cr_assert_eq(result, sizeof(input));
	cr_assert_arr_eq(output, input, sizeof(input));
	cr_assert_eq(output[sizeof(input)], 0);
	utf8proc_free(output);
}
