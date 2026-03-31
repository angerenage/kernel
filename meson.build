project('kernel', 'c',
	version : '0.1.0',
	default_options : ['warning_level=2', 'buildtype=debug']
)

platform = get_option('platform')

freestanding_c_args = [
	'-ffreestanding', '-fno-builtin', '-nostdinc',
]

subdir('toolchain/meson')

platform_kernel_objects = []
platform_kernel_c_args = []
platform_kernel_link_args = []
platform_limine_arch = ''

subdir('base')
subdir('core')
subdir('platforms/' + platform)
subdir('kernel')
subdir('test')

alias_target('kernel', [kernel_elf])

bash = find_program('bash', required : true)
limine_script = files('scripts/make_limine_image.sh')
limine_config = files('boot/limine.conf')

limine_image = custom_target('limine_image',
	input : kernel_elf,
	output : 'kernel.iso',
	command : [
		bash,
		limine_script[0],
		'--arch', platform_limine_arch,
		'--kernel', '@INPUT@',
		'--config', limine_config[0],
		'--output', '@OUTPUT@',
		'--builddir', meson.project_build_root(),
	],
	depend_files : limine_config + limine_script,
	build_by_default : true
)

alias_target('image', [limine_image])
