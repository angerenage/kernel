init_elf = executable('init.elf',
	sources : files('main.c'),
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += init_elf
