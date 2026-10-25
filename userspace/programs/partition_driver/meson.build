partition_driver_sources = files(
	'src/gpt.c',
	'src/main.c',
	'src/mbr.c',
	'src/partition.c',
)

partition_driver_elf = executable('partition_driver.elf',
	sources : partition_driver_sources,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : device_manager_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += partition_driver_elf
