base_libc_sources = files(
	'libc/string/memchr.c',
	'libc/string/memcmp.c',
	'libc/string/memcpy.c',
	'libc/string/memmove.c',
	'libc/string/memset.c',
	'libc/string/strcmp.c',
	'libc/string/strncmp.c',
	'libc/string/strlcpy.c',
	'libc/string/strdup.c',
	'libc/string/strndup.c',
	'libc/string/strlen.c',
	'libc/stdlib/calloc.c',
	'libc/stdlib/free.c',
	'libc/stdlib/malloc.c',
	'libc/stdlib/realloc.c',
	'libc/stdio/format.c',
	'libc/stdio/printf.c',
	'libc/stdio/putchar.c',
	'libc/stdio/puts.c',
	'libc/stdio/sprintf.c',
	'libc/heap.c',
	'libc/math.c',
)

base_io_port_sources = []
if platform == 'pc_x86_64'
	base_io_port_sources += files('io_ports.c')
endif

base_sources = (
	base_libc_sources
	+ files('time.c')
	+ base_io_port_sources
)
