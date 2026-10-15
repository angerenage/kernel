mxml_config_data = configuration_data()
mxml_config_data.set('MXML_VERSION', 'Mini-XML v4.0.4')

configure_file(
	input : 'config.h.meson',
	output : 'config.h',
	configuration : mxml_config_data,
)

mxml_inc = include_directories(
	'.',
	'../../include/libc',
)

mxml_sources = files(
	'mxml-attr.c',
	'mxml-file.c',
	'mxml-get.c',
	'mxml-index.c',
	'mxml-node.c',
	'mxml-options.c',
	'mxml-private.c',
	'mxml-search.c',
	'mxml-set.c',
)

userspace_mxml = static_library(
	'userspace_mxml',
	sources : mxml_sources,
	c_args : freestanding_c_args + ['-fno-pic', '-fno-pie', '-Wno-implicit-fallthrough'] + platform_userspace_c_args,
	include_directories : mxml_inc,
	pic : false,
	install : false,
)

userspace_mxml_dep = declare_dependency(
	link_with : userspace_mxml,
	include_directories : include_directories('.'),
)

