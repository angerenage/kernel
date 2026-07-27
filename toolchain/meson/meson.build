clang = find_program('clang', required : true)

clang_x86_64_elf = generator(clang,
	output : ['@BASENAME@.o'],
	arguments : ['--target=x86_64-elf', '-c', '-x', 'assembler', '-o', '@OUTPUT@', '@INPUT@']
)

clang_aarch64_elf = generator(clang,
	output : ['@BASENAME@.o'],
	arguments : ['--target=aarch64-elf', '-c', '-x', 'assembler', '-o', '@OUTPUT@', '@INPUT@']
)

clang_riscv64_elf = generator(clang,
	output : ['@BASENAME@.o'],
	arguments : ['--target=riscv64-elf', '-march=rv64gcv', '-mabi=lp64',
		'-c', '-x', 'assembler', '-o', '@OUTPUT@', '@INPUT@']
)

clang_loongarch64_elf = generator(clang,
	output : ['@BASENAME@.o'],
	arguments : ['--target=loongarch64-unknown-none-gnusf', '-mlasx',
		'-c', '-x', 'assembler', '-o', '@OUTPUT@', '@INPUT@']
)
