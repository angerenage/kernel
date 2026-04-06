project('kernel', 'c',
	version : '0.1.0',
	default_options : ['warning_level=2', 'buildtype=debug']
)

platform = get_option('platform')
kernel_selftests = get_option('kernel_selftests')
kernel_selftests_autorun = get_option('kernel_selftests_autorun')

if kernel_selftests_autorun and not kernel_selftests
	error('kernel_selftests_autorun requires -Dkernel_selftests=true')
endif

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

if get_option('tests')
	subdir('test')
endif

alias_target('kernel', [kernel_elf])

bash = find_program('bash', required : true)
limine_script = files('scripts/make_limine_image.sh')
limine_cmdline = 'loglevel=debug'
if kernel_selftests_autorun
	limine_cmdline += ' kernel.selftest=1'
endif

limine_conf_data = configuration_data()
limine_conf_data.set('KERNEL_BOOT_CMDLINE', limine_cmdline)

limine_config = configure_file(
	input : 'boot/limine.conf.in',
	output : 'limine.conf',
	configuration : limine_conf_data,
)

limine_image = custom_target('limine_image',
	input : kernel_elf,
	output : 'kernel.iso',
	command : [
		bash,
		limine_script[0],
		'--arch', platform_limine_arch,
		'--kernel', '@INPUT@',
		'--config', limine_config,
		'--output', '@OUTPUT@',
		'--builddir', meson.project_build_root(),
	],
	depend_files : [limine_config, limine_script[0]],
	build_by_default : true
)

alias_target('image', [limine_image])
