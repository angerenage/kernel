input_manager_include = include_directories('include')

input_manager_client = static_library('input_manager_client',
	sources : files('lib/input_manager.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_runtime_inc, input_manager_include],
	pic : false,
	install : false,
)

input_manager_dep = declare_dependency(
	include_directories : input_manager_include,
	link_with : input_manager_client,
)

input_manager_elf = executable('input_manager.elf',
	sources : files(
		'src/input_manager.c',
		'src/main.c',
		'src/server.c',
	),
	c_args : userspace_c_args,
	include_directories : [userspace_inc, input_manager_include],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += input_manager_elf
