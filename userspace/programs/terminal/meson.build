terminal_include = include_directories('include')

terminal_client = static_library('terminal_client',
	sources : files('lib/terminal.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_runtime_inc, terminal_include],
	pic : false,
	install : false,
)

terminal_protocol_dep = declare_dependency(
	include_directories : terminal_include,
)

terminal_dep = declare_dependency(
	include_directories : terminal_include,
	link_with : terminal_client,
)

terminal_elf = executable(
	'terminal.elf',
	sources : files(
		'src/glyph_cache.c',
		'src/input.c',
		'src/main.c',
		'src/server.c',
		'src/terminal.c',
	),
	c_args : userspace_c_args,
	include_directories : [userspace_inc, terminal_include],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	dependencies : [userspace_freetype_dep, input_manager_dep, text_input_dep],
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += terminal_elf
