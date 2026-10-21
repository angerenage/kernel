init_elf = executable('init.elf',
	sources : files(
		'main.c',
		'init_crt0.c',
		'arguments.c',
		'configuration.c',
		'launcher.c',
		'device_manager.c',
		'serial_stream.c',
		'supervisor.c',
	),
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : [loader_dep, vfs_dep, device_manager_dep, terminal_protocol_dep, tty_session_protocol_dep, text_input_protocol_dep, userspace_yaml_dep],
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += init_elf
