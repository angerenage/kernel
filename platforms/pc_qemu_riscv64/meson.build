platform_limine_arch = 'riscv64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_QEMU_RISCV64',
]

platform_kernel_objects += [
	clang_riscv64_elf.process(files('entry.asm')),
	clang_riscv64_elf.process(files('interrupts.asm')),
	files('hal/clock.c'),
	files('hal/hcf.c'),
	files('hal/interrupts.c'),
	files('hal/paging.c'),
	files('hal/serial.c'),
]
