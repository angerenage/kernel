userspace_libc_inc = include_directories('../include/libc', '../include', 'include')
userspace_system_inc = include_directories('../include/libc', '../include', 'include')
userspace_runtime_inc = include_directories(
	'../include/libc',
	'../include',
	'include',
	'programs/device_manager/include',
	'programs/vfs/include',
)
userspace_inc = include_directories('../include/libc', '../include', 'include')

userspace_linker_file = meson.current_source_dir() + '/linker.ld'

userspace_c_args = freestanding_c_args + [
	'-DBASE_FORMAT_FLOAT=1',
	'-fno-pic',
	'-fno-pie',
]
userspace_c_args += platform_userspace_c_args

if get_option('debug')
	userspace_c_args += ['-DRUNTIME_DIAGNOSTICS=1']
endif

if kernel_boot_debug
	userspace_c_args += ['-DBOOT_DEBUG_LOGGING=1']
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

userspace_libc_sources = files(
	'libc/locale.c',
	'libc/strtod.c',
)

if platform == 'pc_x86_64'
	userspace_setjmp_object = clang_x86_64_elf.process(files('platforms/pc_x86_64/setjmp.asm'))
elif platform == 'pc_aarch64'
	userspace_setjmp_object = clang_aarch64_elf.process(files('platforms/pc_aarch64/setjmp.asm'))
elif platform == 'pc_riscv64'
	userspace_setjmp_object = clang_riscv64_userspace_elf.process(files('platforms/pc_riscv64/setjmp.asm'))
elif platform == 'pc_loongarch64'
	userspace_setjmp_object = clang_loongarch64_userspace_elf.process(files('platforms/pc_loongarch64/setjmp.asm'))
endif

userspace_runtime_sources = files(
	'runtime/diagnostic.c',
	'runtime/block.c',
	'runtime/filesystem.c',
	'runtime/file_output.c',
	'runtime/heap.c',
	'runtime/registry.c',
	'runtime/input.c',
	'runtime/blob.c',
	'runtime/stdio.c',
	'runtime/startup.c',
	'runtime/stream.c',
	'runtime/unistd.c',
)

userspace_system_sources = files(
	'system/acpi.c',
	'system/capability.c',
	'system/channel.c',
	'system/device_tree.c',
	'system/display.c',
	'system/framebuffer.c',
	'system/kernel_resource.c',
	'system/loader.c',
	'system/memory.c',
	'system/module.c',
	'system/dma.c',
	'system/interrupt.c',
	'system/process.c',
	'system/signal.c',
	'system/thread.c',
	'system/time.c',
	'system/trace.c',
	'system/upcall.c',
	'platforms/' + platform + '/syscall.c',
)
if platform == 'pc_x86_64'
	userspace_system_sources += base_io_port_sources + files('system/io_port.c')
endif

userspace_libc = static_library('userspace_libc',
	sources : base_libc_sources + userspace_libc_sources + userspace_base_sources + [userspace_setjmp_object],
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

subdir('programs/vfs')
userspace_link_libs += vfs_client
subdir('programs/loader')
subdir('programs/device_manager')
subdir('programs/input_manager')
subdir('programs/keyboard_controller')
subdir('programs/text_input')
subdir('programs/terminal')
subdir('programs/tty_session')
subdir('programs/ls')
subdir('programs/cat')
subdir('programs/shell')
subdir('programs/registry')
subdir('programs/init')
subdir('programs/bootfs')
subdir('programs/acpi_parser')
subdir('programs/dt_parser')
subdir('programs/pci_driver')
subdir('programs/ahci_driver')
subdir('programs/sata_driver')
subdir('programs/partition_driver')
subdir('programs/fat32_driver')
subdir('programs/ps2_driver')
if platform == 'pc_x86_64'
	subdir('programs/i8042_driver')
endif

driver_catalog = configure_file(
	input : 'config/drivers.yaml',
	output : 'drivers.yaml',
	copy : true,
)

boot_driver_catalog = configure_file(
	input : 'config/boot_drivers.yaml',
	output : 'boot-drivers.yaml',
	copy : true,
)

system_configuration = configure_file(
	input : 'config/system.yaml',
	output : 'system.yaml',
	copy : true,
)

keyboard_qwerty_layout = configure_file(
	input : 'config/keyboard_qwerty.xml',
	output : 'keyboard_qwerty.xml',
	copy : true,
)

keyboard_azerty_layout = configure_file(
	input : 'config/keyboard_azerty.xml',
	output : 'keyboard_azerty.xml',
	copy : true,
)

userspace_boot_programs = [
	init_elf,
	registry_elf,
	vfs_elf,
	bootfs_elf,
	loader_elf,
	device_manager_elf,
	terminal_elf,
	acpi_parser_elf,
	dt_parser_elf,
	pci_driver_elf,
	ahci_driver_elf,
	sata_driver_elf,
	partition_driver_elf,
	fat32_driver_elf,
]

userspace_boot_modules = userspace_boot_programs + [
	boot_driver_catalog,
	system_configuration,
	commit_mono_regular_font,
]

userspace_root_programs = userspace_programs
userspace_root_files = [
	[driver_catalog, 'etc/drivers.yaml'],
	[system_configuration, 'etc/system.yaml'],
	[keyboard_qwerty_layout, 'etc/keyboard_qwerty.xml'],
	[keyboard_azerty_layout, 'etc/keyboard_azerty.xml'],
	[commit_mono_regular_font, 'share/fonts/CommitMono.otf'],
	[commit_mono_license, 'share/licenses/CommitMono-LICENSE.txt'],
]

alias_target(
	'userspace',
	userspace_programs + [userspace_utf8proc, userspace_yaml, userspace_mxml, userspace_freetype],
)
