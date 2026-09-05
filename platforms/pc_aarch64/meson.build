platform_limine_arch = 'aarch64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_AARCH64',
	'-mgeneral-regs-only',
]

platform_userspace_c_args += [
	'-march=armv8-a+simd',
]

platform_kernel_objects += [
	clang_aarch64_elf.process(files('entry.asm')),
	clang_aarch64_elf.process(files('interrupts.asm')),
	clang_aarch64_elf.process(files('hal/userspace.asm')),
	clang_aarch64_elf.process(files('thread.asm')),
	files('hal/cache.c'),
	files('hal/cpu.c'),
	files('hal/clock.c'),
	files('hal/hcf.c'),
	files('hal/interrupts.c'),
	files('hal/paging.c'),
	files('hal/serial.c'),
	files('hal/syscall.c'),
	files('hal/userspace.c'),
]
