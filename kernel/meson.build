linker_file = meson.current_source_dir() + '/linker.ld'

kernel_c_args = freestanding_c_args + [
	'-fno-pic', '-fno-pie',
]
kernel_c_args += platform_kernel_c_args
kernel_c_args += '-DKERNEL_SELFTESTS_ENABLED=' + (kernel_selftests ? '1' : '0')
kernel_c_args += '-DCORE_LOCK_DEBUG=' + (kernel_selftests ? '1' : '0')
kernel_ld_args = [
	'-nostdlib',
	'-Wl,-z,max-page-size=0x1000',
	'-Wl,-T,' + linker_file,
	'-Wl,-no-pie',
	'-Wl,--build-id=sha1',
]
kernel_ld_args += platform_kernel_link_args

kernel_inc = [include_directories('../include/libc', '../include', '../'), qrcodegen_inc]

kernel_build_config_data = configuration_data()
kernel_build_config_data.set('KERNEL_GIT_COMMIT', '@KERNEL_GIT_COMMIT@')
kernel_build_config_data.set('KERNEL_CRASH_SERVER_URL', crash_server_url)
kernel_build_config_template = configure_file(
	input: 'config.h.in',
	output: 'kernel_config.h.in',
	configuration: kernel_build_config_data,
)

git_program = find_program('git', native: true, required: false)
if git_program.found()
	kernel_build_config = vcs_tag(
		input: kernel_build_config_template,
		output: 'kernel_config.h',
		command: [git_program, 'rev-parse', '--verify', 'HEAD'],
		fallback: 'unknown',
		replace_string: '@KERNEL_GIT_COMMIT@',
	)
else
	kernel_build_config_data = configuration_data()
	kernel_build_config_data.set('KERNEL_GIT_COMMIT', 'unknown')
	kernel_build_config_data.set('KERNEL_CRASH_SERVER_URL', crash_server_url)
	kernel_build_config = configure_file(
		input: 'config.h.in',
		output: 'kernel_config.h',
		configuration: kernel_build_config_data,
	)
endif

kernel_libk_sources = files(
	'libk/stdio/display.c',
	'libk/stdlib/heap.c',
)

kernel_selftest_sources = files(
	'test/selftest.c',
	'test/selftests/cpu.c',
	'test/selftests/thread_bootstrap.c',
	'test/selftests/pmm.c',
	'test/selftests/iommu.c',
	'test/selftests/address_space.c',
	'test/selftests/heap.c',
	'test/selftests/kthread.c',
	'test/selftests/sched_mutex.c',
	'test/selftests/semaphore.c',
	'test/selftests/condvar.c',
	'test/selftests/rwlock.c',
)

kernel_objs = base_sources + core_sources + boot_sources + firmware_sources + kernel_libk_sources + qrcodegen_sources + platform_kernel_objects + [
	kernel_build_config,
	'src/crash.c',
	'src/crash_qr.c',
	'src/crash_record.c',
	'src/boot/cmdline.c',
	'src/boot/diagnostics.c',
	'src/boot/cpu_boot.c',
	'src/capability/capability.c',
	'src/capability/acpi.c',
	'src/capability/address_space.c',
	'src/capability/memory.c',
	'src/capability/memory_allocator.c',
	'src/capability/boot_module.c',
	'src/capability/boot_resource.c',
	'src/capability/device_tree.c',
	'src/capability/kernel_resource.c',
	'src/capability/loader.c',
	'src/capability/process.c',
	'src/capability/serial.c',
	'src/capability/signal.c',
	'src/capability/thread.c',
	'src/capability/dma.c',
	'src/capability/interrupt.c',
	'src/elf_loader.c',
	'src/init.c',
	'src/syscall/capability.c',
	'src/syscall/channel.c',
	'src/syscall/misc.c',
	'src/syscall/process.c',
	'src/syscall/signal.c',
	'src/syscall/syscall.c',
	'src/syscall/thread.c',
	'src/syscall/time.c',
	'src/syscall/upcall.c',
	'src/syscall/user_frame.c',
]
if platform == 'pc_x86_64'
	kernel_objs += files('src/capability/io_port.c')
endif
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
