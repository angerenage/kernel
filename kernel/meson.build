linker_file = meson.current_source_dir() + '/linker.ld'

kernel_c_args = [
	'-ffreestanding', '-fno-builtin',
	'-mno-red-zone', '-mcmodel=kernel',
	'-fno-pic', '-fno-pie',
]
kernel_c_args += platform_kernel_c_args
kernel_ld_args = [
	'-nostdlib',
	'-Wl,-z,max-page-size=0x1000',
	'-Wl,-T,' + linker_file,
	'-Wl,-no-pie',
]
kernel_ld_args += platform_kernel_link_args

kernel_inc = include_directories('include', '../')

kernel_objs = [
	'src/init.c',
	'src/requests.c',
	'libk/stdio/utils.c',
	'libk/stdio/putchar.c',
	'libk/stdio/puts.c',
	'libk/stdio/printf.c',
	'libk/stdio/sprintf.c',
	'libk/string/memcmp.c',
	'libk/string/memcpy.c',
	'libk/string/memmove.c',
	'libk/string/memset.c',
	'libk/string/strlen.c',
] + platform_kernel_objects

kernel_elf = executable('kernel.elf',
	sources : kernel_objs,
	c_args : kernel_c_args,
	include_directories : kernel_inc,
	link_args : kernel_ld_args,
	install : false,
	link_depends : files(linker_file),
)
