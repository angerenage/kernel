core_libk_sources = files(
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
)

core_kernel_sources = files(
	'memory/early_alloc.c',
	'memory/pmm.c',
	'memory/vaddr_alloc.c',
	'mm/boot_info.c',
)

core_kernel_runtime_sources = core_kernel_sources + files(
	'memory/vmm.c',
)

core_sources = core_libk_sources + core_kernel_runtime_sources
