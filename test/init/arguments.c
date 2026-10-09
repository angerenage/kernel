#include "../../userspace/programs/init/arguments.h"

#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

Test(init_arguments, validates_exact_argument_payload) {
	static const char valid[]        = "first\0second=value\0third\0";
	static const char unterminated[] = {'f', 'i', 'r', 's', 't'};

	cr_assert(init_arguments_valid(3u, valid, sizeof(valid) - 1u));
	cr_assert_not(init_arguments_valid(2u, valid, sizeof(valid) - 1u));
	cr_assert_not(init_arguments_valid(4u, valid, sizeof(valid) - 1u));
	cr_assert_not(init_arguments_valid(1u, unterminated, sizeof(unterminated)));
	cr_assert_not(init_arguments_valid(1u, NULL, 1u));
	cr_assert_not(init_arguments_valid(0u, valid, sizeof(valid) - 1u));
	cr_assert(init_arguments_valid(0u, NULL, 0u));
}

Test(init_arguments, creates_a_vector_without_copying_strings) {
	static const char payload[] = "first\0second=value\0third\0";
	struct init_state init      = {.argc = 3u, .argv_data = payload, .argv_size = sizeof(payload) - 1u};
	const char**      argv      = NULL;

	cr_assert(init_arguments_vector(&init, &argv));
	cr_assert_not_null(argv);
	cr_assert_eq(argv[0], payload);
	cr_assert_eq(argv[1], payload + sizeof("first"));
	cr_assert_eq(argv[2], payload + sizeof("first") + sizeof("second=value"));
	cr_assert_str_eq(argv[0], "first");
	cr_assert_str_eq(argv[1], "second=value");
	cr_assert_str_eq(argv[2], "third");
	free(argv);
}

Test(init_arguments, handles_empty_and_invalid_state) {
	struct init_state empty   = {0};
	struct init_state invalid = {.argc = 1u, .argv_data = "missing terminator", .argv_size = 4u};
	const char**      argv    = (const char**)1;

	cr_assert(init_arguments_vector(&empty, &argv));
	cr_assert_null(argv);
	cr_assert_not(init_arguments_vector(&invalid, &argv));
	cr_assert_null(argv);
	cr_assert_not(init_arguments_vector(NULL, &argv));
	cr_assert_null(argv);
	cr_assert_not(init_arguments_vector(&empty, NULL));
}
