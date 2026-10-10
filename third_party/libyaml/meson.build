libyaml_config = configuration_data()
libyaml_config.set('YAML_VERSION_MAJOR', 0)
libyaml_config.set('YAML_VERSION_MINOR', 2)
libyaml_config.set('YAML_VERSION_PATCH', 5)
libyaml_config.set('YAML_VERSION_STRING', '0.2.5')

configure_file(
	input : 'cmake/config.h.in',
	output : 'config.h',
	configuration : libyaml_config,
)

libyaml_inc = include_directories(
	'.',
	'include',
	'src',
	'../../include/libc',
)

libyaml_sources = files(
	'src/api.c',
	'src/dumper.c',
	'src/emitter.c',
	'src/loader.c',
	'src/parser.c',
	'src/reader.c',
	'src/scanner.c',
	'src/writer.c',
)

userspace_yaml = static_library('userspace_yaml',
	sources : libyaml_sources,
	c_args : freestanding_c_args + ['-fno-pic', '-fno-pie', '-DHAVE_CONFIG_H=1'] + platform_userspace_c_args,
	include_directories : libyaml_inc,
	pic : false,
	install : false,
)

userspace_yaml_dep = declare_dependency(
	link_with : userspace_yaml,
	include_directories : include_directories('include'),
)
