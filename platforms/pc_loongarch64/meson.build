platform_limine_arch = 'loongarch64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_LOONGARCH64',
]

platform_kernel_objects += [
	clang_loongarch64_elf.process(files('entry.asm')),
	clang_loongarch64_elf.process(files('interrupts.asm')),
	files('hal/cpu.c'),
	files('hal/clock.c'),
	files('hal/hcf.c'),
	files('hal/interrupts.c'),
	files('hal/paging.c'),
	files('hal/serial.c'),
]
