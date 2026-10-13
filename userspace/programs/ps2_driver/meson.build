ps2_include = include_directories('include')

ps2_dep = declare_dependency(
	include_directories : ps2_include,
)

ps2_driver_sources = files(
	'src/events.c',
	'src/keyboard.c',
	'src/main.c',
	'src/mouse.c',
	'src/ps2.c',
)

ps2_driver_elf = executable('ps2_driver.elf',
	sources : ps2_driver_sources,
	c_args : userspace_c_args,
	include_directories : [userspace_inc, ps2_include],
	dependencies : device_manager_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += ps2_driver_elf
