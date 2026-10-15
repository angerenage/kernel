text_input_include = include_directories('include')

text_input_client = static_library('text_input_client',
	sources : files('lib/text_input.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_runtime_inc, text_input_include],
	dependencies : userspace_utf8proc_dep,
	pic : false,
	install : false,
)

text_input_dep = declare_dependency(
	include_directories : text_input_include,
	link_with : text_input_client,
	dependencies : userspace_utf8proc_dep,
)

text_input_elf = executable('text_input.elf',
	sources : files(
		'src/layout.c',
		'src/main.c',
		'src/server.c',
		'src/text_input.c',
	),
	c_args : userspace_c_args,
	include_directories : [userspace_inc, text_input_include],
	dependencies : [keyboard_controller_dep, userspace_mxml_dep, userspace_utf8proc_dep],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += text_input_elf
