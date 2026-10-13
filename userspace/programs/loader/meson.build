loader_include = include_directories('include')

loader_client = static_library('loader_client',
	sources : files('lib/program.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_runtime_inc, loader_include],
	pic : false,
	install : false,
)

loader_dep = declare_dependency(
	include_directories : loader_include,
	link_with : loader_client,
)

loader_elf = executable('loader.elf',
	sources : files('main.c', 'elf64.c', 'lifecycle.c', 'load.c', 'load_plan.c', 'server.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_inc, loader_include],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += loader_elf
