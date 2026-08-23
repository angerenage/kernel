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
]
kernel_ld_args += platform_kernel_link_args

kernel_inc = include_directories('../include/libc', '../include', '../')

kernel_libk_sources = files(
	'libk/stdio/display.c',
	'libk/stdlib/heap.c',
)

kernel_selftest_sources = files(
	'test/selftest.c',
	'test/selftests/cpu.c',
	'test/selftests/thread_bootstrap.c',
	'test/selftests/pmm.c',
	'test/selftests/vmm.c',
	'test/selftests/heap.c',
	'test/selftests/kthread.c',
	'test/selftests/sched_mutex.c',
	'test/selftests/semaphore.c',
	'test/selftests/condvar.c',
	'test/selftests/rwlock.c',
)

kernel_objs = base_sources + core_sources + kernel_libk_sources + platform_kernel_objects + [
	'src/boot/boot.c',
	'src/boot/cmdline.c',
	'src/boot/diagnostics.c',
	'src/boot/cpu_boot.c',
	'src/capability/capability.c',
	'src/capability/address_space.c',
	'src/capability/memory.c',
	'src/capability/boot_module.c',
	'src/capability/loader.c',
	'src/capability/process.c',
	'src/capability/serial.c',
	'src/capability/signal.c',
	'src/capability/thread.c',
	'src/elf_loader.c',
	'src/init.c',
	'src/syscall/capability.c',
	'src/syscall/channel.c',
	'src/syscall/memory.c',
	'src/syscall/misc.c',
	'src/syscall/module.c',
	'src/syscall/process.c',
	'src/syscall/signal.c',
	'src/syscall/syscall.c',
	'src/syscall/thread.c',
	'src/syscall/time.c',
	'src/syscall/upcall.c',
	'src/syscall/user_frame.c',
	'src/boot/requests.c',
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
