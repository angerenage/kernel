#include <base/startup.h>
#include <criterion/criterion.h>
#include <runtime/startup.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

Test(runtime_startup, empty_arguments_are_valid) {
	struct {
		struct process_startup_info startup;
		cap_id_t                    capv[PROCESS_STARTUP_CAP_COUNT];
	} payload = {
		.startup =
			{
					  .size        = sizeof(payload),
					  .capc        = PROCESS_STARTUP_CAP_COUNT,
					  .capv_offset = sizeof(struct process_startup_info),
					  },
		.capv = {CAP_ID_INVALID, CAP_ID_INVALID, CAP_ID_INVALID},
	};
	struct runtime_startup_arguments arguments;

	cr_assert_eq(runtime_startup_unpack(&payload.startup, &arguments), RUNTIME_STARTUP_OK);
	cr_assert_eq(arguments.argc, 0);
	cr_assert_null(arguments.argv);
	cr_assert_eq(arguments.stdin_cap, CAP_ID_INVALID);
	cr_assert_eq(arguments.stdout_cap, CAP_ID_INVALID);
	cr_assert_eq(arguments.stderr_cap, CAP_ID_INVALID);
	cr_assert_eq(arguments.capc, 0u);
	cr_assert_null(arguments.capv);
	runtime_startup_arguments_deinit(&arguments);
}

Test(runtime_startup, standard_streams_are_intercepted_and_application_capabilities_are_preserved) {
	struct {
		struct process_startup_info startup;
		cap_id_t                    capv[5];
		char                        argv[10];
	} payload = {
		.startup =
			{
					  .size        = offsetof(typeof(payload), argv) + sizeof(payload.argv),
					  .capc        = 5u,
					  .capv_offset = sizeof(struct process_startup_info),
					  .argc        = 2u,
					  .argv_offset = offsetof(typeof(payload), argv),
					  .argv_size   = sizeof(payload.argv),
					  },
		.capv = {11u, 12u, 13u, CAP_ID_INVALID, 33u},
		.argv = "one\0three",
	};
	struct runtime_startup_arguments arguments;

	cr_assert_eq(runtime_startup_unpack(&payload.startup, &arguments), RUNTIME_STARTUP_OK);
	cr_assert_eq(arguments.stdin_cap, 11u);
	cr_assert_eq(arguments.stdout_cap, 12u);
	cr_assert_eq(arguments.stderr_cap, 13u);
	cr_assert_eq(arguments.capc, 2u);
	cr_assert_eq(arguments.capv[0], CAP_ID_INVALID);
	cr_assert_eq(arguments.capv[1], 33u);
	cr_assert_eq(arguments.argc, 2);
	cr_assert_str_eq(arguments.argv[0], "one");
	cr_assert_str_eq(arguments.argv[1], "three");
	cr_assert_null(arguments.argv[2]);
	runtime_startup_arguments_deinit(&arguments);
}

Test(runtime_startup, malformed_layouts_are_rejected) {
	struct {
		struct process_startup_info startup;
		cap_id_t                    capv[PROCESS_STARTUP_CAP_COUNT + 1u];
		char                        argv[4];
	} payload = {
		.startup =
			{
					  .size        = offsetof(typeof(payload), argv) + sizeof(payload.argv),
					  .capc        = PROCESS_STARTUP_CAP_COUNT + 1u,
					  .capv_offset = sizeof(struct process_startup_info),
					  .argc        = 1u,
					  .argv_offset = offsetof(typeof(payload), argv),
					  .argv_size   = sizeof(payload.argv),
					  },
		.capv = {1u, 2u, 3u, 9u},
		.argv = "arg",
	};
	struct runtime_startup_arguments arguments;

	payload.startup.capc = PROCESS_STARTUP_CAP_COUNT - 1u;
	cr_assert_eq(runtime_startup_unpack(&payload.startup, &arguments), RUNTIME_STARTUP_INVALID);
	payload.startup.capc = PROCESS_STARTUP_CAP_COUNT + 1u;
	payload.startup.capv_offset++;
	cr_assert_eq(runtime_startup_unpack(&payload.startup, &arguments), RUNTIME_STARTUP_INVALID);
	payload.startup.capv_offset = sizeof(struct process_startup_info);
	payload.argv[3]             = 'x';
	cr_assert_eq(runtime_startup_unpack(&payload.startup, &arguments), RUNTIME_STARTUP_INVALID);
	payload.argv[3] = '\0';
	payload.startup.size--;
	cr_assert_eq(runtime_startup_unpack(&payload.startup, &arguments), RUNTIME_STARTUP_INVALID);
}
