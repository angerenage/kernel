utf8proc_inc = include_directories(
	'utf8proc',
	'../../include/libc',
)

utf8proc_sources = files('utf8proc/utf8proc.c')

userspace_utf8proc = static_library(
	'userspace_utf8proc',
	sources : utf8proc_sources,
	c_args : freestanding_c_args + [
		'-fno-pic',
		'-fno-pie',
		'-DUTF8PROC_STATIC',
	] + platform_userspace_c_args,
	include_directories : utf8proc_inc,
	pic : false,
	install : false,
)

userspace_utf8proc_dep = declare_dependency(
	link_with : userspace_utf8proc,
	include_directories : include_directories('utf8proc'),
	compile_args : ['-DUTF8PROC_STATIC'],
)
