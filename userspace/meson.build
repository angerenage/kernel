userspace_libc_inc = include_directories('../include/libc', '../include')
userspace_runtime_inc = include_directories('../include/libc', '../include', 'include')
userspace_inc = include_directories('../include/libc', 'include')

userspace_linker_file = meson.current_source_dir() + '/linker.ld'

userspace_c_args = freestanding_c_args + [
	'-fno-pic',
	'-fno-pie',
]

if platform == 'pc_x86_64'
	userspace_c_args += [
		'-mno-red-zone',
		'-mstackrealign',
		'-mcmodel=small',
	]
endif

userspace_link_args = [
	'-nostdlib',
	'-Wl,-z,max-page-size=0x1000',
	'-Wl,-T,' + userspace_linker_file,
	'-Wl,-no-pie',
]

userspace_crt_sources = files(
	'base/crt0.c',
)

userspace_runtime_sources = files(
	'base/display.c',
	'platforms/' + platform + '/syscall.c',
)

userspace_libc = static_library('userspace_libc',
	sources : base_libc_sources,
	c_args : userspace_c_args,
	include_directories : userspace_libc_inc,
	pic : false,
	install : false,
)

userspace_runtime = static_library('userspace_runtime',
	sources : userspace_runtime_sources,
	c_args : userspace_c_args,
	include_directories : userspace_runtime_inc,
	pic : false,
	install : false,
)

userspace_crt = static_library('userspace_crt',
	sources : userspace_crt_sources,
	c_args : userspace_c_args,
	include_directories : userspace_runtime_inc,
	pic : false,
	install : false,
)

userspace_link_libs = [
	userspace_libc,
	userspace_runtime,
]

userspace_programs = []

subdir('programs/init')

alias_target('userspace', userspace_programs)
