i8042_driver_sources = files(
	'src/i8042.c',
	'src/main.c',
)

i8042_driver_elf = executable('i8042_driver.elf',
	sources : i8042_driver_sources,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : [device_manager_dep, ps2_dep],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += i8042_driver_elf
