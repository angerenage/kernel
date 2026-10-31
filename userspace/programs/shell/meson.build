shell_elf = executable('shell.elf',
	sources : files(
		'src/command.c',
		'src/parse.c',
		'src/editor.c',
		'src/filesystem.c',
		'src/input.c',
		'src/main.c',
		'src/program.c',
		'src/shell.c',
	),
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : [loader_dep, tty_session_dep, vfs_dep],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += shell_elf
