platform_limine_arch = 'aarch64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_QEMU_AARCH64',
]

platform_kernel_objects += [
	clang_aarch64_elf.process(files('entry.asm')),
	clang_aarch64_elf.process(files('interrupts.asm')),
	files('hal/hcf.c'),
	files('hal/interrupts.c'),
	files('hal/serial.c'),
]
