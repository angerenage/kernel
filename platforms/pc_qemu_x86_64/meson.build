platform_limine_arch = 'x86_64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_QEMU_X86_64',
	'-mno-red-zone',
	'-mcmodel=kernel',
]

platform_kernel_objects += [
	clang_x86_64_elf.process(files('entry.asm')),
	clang_x86_64_elf.process(files('interrupts.asm')),
	files('hal/hcf.c'),
	files('hal/interrupts.c'),
	files('hal/serial.c'),
]
