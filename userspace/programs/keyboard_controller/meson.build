keyboard_controller_include = include_directories('include')

keyboard_controller_client = static_library('keyboard_controller_client',
	sources : files('lib/keyboard.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_runtime_inc, keyboard_controller_include],
	pic : false,
	install : false,
)

keyboard_controller_dep = declare_dependency(
	include_directories : keyboard_controller_include,
	link_with : keyboard_controller_client,
)

keyboard_controller_elf = executable('keyboard_controller.elf',
	sources : files(
		'src/keyboard_controller.c',
		'src/main.c',
		'src/server.c',
	),
	c_args : userspace_c_args,
	include_directories : [userspace_inc, keyboard_controller_include],
	dependencies : input_manager_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += keyboard_controller_elf
