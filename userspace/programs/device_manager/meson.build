device_manager_src = files(
	'src/main.c',
)

device_manager_elf = executable('device_manager.elf',
	sources : device_manager_src,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += device_manager_elf
