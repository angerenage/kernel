loader_elf = executable('loader.elf',
	sources : files('main.c', 'elf64.c', 'lifecycle.c', 'load.c', 'server.c'),
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += loader_elf
