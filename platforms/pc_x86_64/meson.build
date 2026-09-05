platform_limine_arch = 'x86_64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_X86_64',
	'-mno-red-zone',
	'-mcmodel=kernel',
	'-mno-80387',
	'-mno-mmx',
	'-mno-sse',
	'-mno-sse2',
]

platform_userspace_c_args += [
	'-msse2',
	'-mfpmath=sse',
	'-mno-avx',
	'-mno-avx2',
	'-mno-avx512f',
]

platform_kernel_objects += [
	clang_x86_64_elf.process(files('entry.asm')),
	clang_x86_64_elf.process(files('interrupts.asm')),
	clang_x86_64_elf.process(files('hal/syscall.asm')),
	clang_x86_64_elf.process(files('hal/userspace.asm')),
	clang_x86_64_elf.process(files('thread.asm')),
	files('hal/cache.c'),
	files('hal/cpu.c'),
	files('hal/clock.c'),
	files('hal/hcf.c'),
	files('hal/interrupts_apic.c'),
	files('hal/interrupts.c'),
	files('hal/interrupts_pic.c'),
	files('hal/paging.c'),
	files('hal/serial.c'),
	files('hal/syscall.c'),
	files('hal/userspace.c'),
]
