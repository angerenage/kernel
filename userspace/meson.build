userspace_libc_inc = include_directories('../include/libc', '../include', 'include')
userspace_system_inc = include_directories('../include/libc', '../include', 'include')
userspace_runtime_inc = include_directories('../include/libc', '../include', 'include')
userspace_inc = include_directories('../include/libc', '../include', 'include')

userspace_linker_file = meson.current_source_dir() + '/linker.ld'

userspace_c_args = freestanding_c_args + [
	'-fno-pic',
	'-fno-pie',
]
userspace_c_args += platform_userspace_c_args

if get_option('debug')
	userspace_c_args += ['-DRUNTIME_DIAGNOSTICS=1']
endif

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
	'runtime/crt0.c',
)

userspace_base_sources = files(
	'base/display.c',
	'base/heap.c',
)

userspace_runtime_sources = files(
	'runtime/diagnostic.c',
	'runtime/heap.c',
	'runtime/init.c',
	'runtime/blob.c',
	'runtime/program.c',
	'runtime/startup.c',
)

userspace_system_sources = files(
	'system/boot_data.c',
	'system/capability.c',
	'system/channel.c',
	'system/display.c',
	'system/framebuffer.c',
	'system/interrupt.c',
	'system/kernel_resource.c',
	'system/loader.c',
	'system/memory.c',
	'system/module.c',
	'system/process.c',
	'system/signal.c',
	'system/thread.c',
	'system/time.c',
	'system/upcall.c',
	'platforms/' + platform + '/syscall.c',
)

userspace_libc = static_library('userspace_libc',
	sources : base_libc_sources + userspace_base_sources,
	c_args : userspace_c_args,
	include_directories : userspace_libc_inc,
	pic : false,
	install : false,
)

userspace_system = static_library('userspace_system',
	sources : userspace_system_sources,
	c_args : userspace_c_args,
	include_directories : userspace_system_inc,
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
	userspace_runtime,
	userspace_libc,
	userspace_system,
]

userspace_programs = []

subdir('programs/init')
subdir('programs/loader')

alias_target('userspace', userspace_programs)
