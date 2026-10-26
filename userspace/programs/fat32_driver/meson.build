fat32_driver_sources = files(
	'src/directory.c',
	'src/fat32.c',
	'src/main.c',
	'src/server.c',
	'src/volume.c',
	'src/write.c',
)

fat32_driver_elf = executable('fat32_driver.elf',
	sources : fat32_driver_sources,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : device_manager_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += fat32_driver_elf
