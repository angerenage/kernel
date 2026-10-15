#include <criterion/criterion.h>
#include <ft2build.h>
#include FT_FREETYPE_H

Test(freetype, initializes_the_pinned_release) {
	FT_Int     major;
	FT_Int     minor;
	FT_Int     patch;
	FT_Library library = NULL;

	cr_assert_eq(FT_Init_FreeType(&library), FT_Err_Ok);
	cr_assert_not_null(library);
	FT_Library_Version(library, &major, &minor, &patch);
	cr_assert_eq(major, 2);
	cr_assert_eq(minor, 15);
	cr_assert_eq(patch, 0);
	cr_assert_eq(FT_Done_FreeType(library), FT_Err_Ok);
}

Test(freetype, rejects_invalid_memory_faces) {
	static const FT_Byte invalid_font[] = "not a font";
	FT_Library           library        = NULL;
	FT_Face              face           = NULL;

	cr_assert_eq(FT_Init_FreeType(&library), FT_Err_Ok);
	cr_assert_neq(FT_New_Memory_Face(library, invalid_font, sizeof(invalid_font), 0, &face), FT_Err_Ok);
	cr_assert_null(face);
	cr_assert_eq(FT_Done_FreeType(library), FT_Err_Ok);
}
