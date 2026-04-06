linker_file = meson.current_source_dir() + '/linker.ld'

kernel_c_args = freestanding_c_args + [
	'-fno-pic', '-fno-pie',
]
kernel_c_args += platform_kernel_c_args
kernel_c_args += '-DKERNEL_SELFTESTS_ENABLED=' + (kernel_selftests ? '1' : '0')
kernel_ld_args = [
	'-nostdlib',
	'-Wl,-z,max-page-size=0x1000',
	'-Wl,-T,' + linker_file,
	'-Wl,-no-pie',
]
kernel_ld_args += platform_kernel_link_args

kernel_inc = include_directories('../include/libk', '../include', '../')

kernel_format_sources = files(
	'libk/format/format.c',
	'libk/format/sprintf.c',
)

kernel_libk_sources = files(
	'libk/stdio/printf.c',
	'libk/stdio/putchar.c',
	'libk/stdio/puts.c',
)

kernel_memory_sources = files(
	'src/memory/kheap_vmm.c',
	'src/memory/vmm.c',
)

kernel_selftest_sources = files(
	'test/selftest.c',
	'test/selftests/pmm.c',
	'test/selftests/vmm.c',
	'test/selftests/kheap.c',
)

kernel_objs = base_sources + core_sources + kernel_format_sources + kernel_libk_sources + kernel_memory_sources + platform_kernel_objects + [
	'src/init.c',
	'src/requests.c',
]
if kernel_selftests
	kernel_objs += kernel_selftest_sources
endif

kernel_elf = executable('kernel.elf',
	sources : kernel_objs,
	c_args : kernel_c_args,
	include_directories : kernel_inc,
	link_args : kernel_ld_args,
	install : false,
	link_depends : files(linker_file),
)
