dt_parser_src = files(
	'src/main.c',
	'src/parser.c',
	'src/property.c',
	'src/tree.c',
)

dt_parser_elf = executable('dt_parser.elf',
	sources : dt_parser_src,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += dt_parser_elf
