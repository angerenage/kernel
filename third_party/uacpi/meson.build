uacpi_inc = include_directories(
	'uacpi/include',
	'adapter/include',
	'../../include',
	'../../include/libc',
	'../../userspace/include',
)

# Keep this list aligned with the upstream Meson source list. The uACPI
# submodule remains pristine so updates can be reviewed independently from
# project-specific build and platform integration.
uacpi_sources = files(
	'uacpi/source/tables.c',
	'uacpi/source/types.c',
	'uacpi/source/uacpi.c',
	'uacpi/source/utilities.c',
	'uacpi/source/interpreter.c',
	'uacpi/source/opcodes.c',
	'uacpi/source/namespace.c',
	'uacpi/source/stdlib.c',
	'uacpi/source/opregion.c',
	'uacpi/source/default_handlers.c',
	'uacpi/source/io.c',
	'uacpi/source/notify.c',
	'uacpi/source/sleep.c',
	'uacpi/source/registers.c',
	'uacpi/source/resources.c',
	'uacpi/source/event.c',
	'uacpi/source/mutex.c',
	'uacpi/source/osi.c',
)

uacpi_adapter_sources = files(
	'adapter/source/aml.c',
	'adapter/source/platform.c',
)

userspace_uacpi_upstream = static_library(
	'userspace_uacpi',
	sources : uacpi_sources,
	c_args : freestanding_c_args + ['-fno-pic', '-fno-pie'] + platform_userspace_c_args,
	include_directories : uacpi_inc,
	pic : false,
	install : false,
)

userspace_uacpi_adapter = static_library(
	'userspace_uacpi_adapter',
	sources : uacpi_adapter_sources,
	c_args : freestanding_c_args + ['-fno-pic', '-fno-pie'] + platform_userspace_c_args,
	include_directories : uacpi_inc,
	pic : false,
	install : false,
)

userspace_uacpi_dep = declare_dependency(
	link_with : [userspace_uacpi_adapter, userspace_uacpi_upstream],
	include_directories : include_directories('uacpi/include', 'adapter/include'),
)
