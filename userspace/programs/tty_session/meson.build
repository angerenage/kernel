tty_session_include = include_directories('include')

tty_session_client = static_library('tty_session_client',
	sources : files('lib/tty_session.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_runtime_inc, tty_session_include],
	pic : false,
	install : false,
)

tty_session_protocol_dep = declare_dependency(
	include_directories : tty_session_include,
)

tty_session_dep = declare_dependency(
	include_directories : tty_session_include,
	link_with : tty_session_client,
)

tty_session_elf = executable('tty_session.elf',
	sources : files('src/main.c', 'src/session.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_inc, tty_session_include],
	dependencies : terminal_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += tty_session_elf
