nasm = find_program('nasm', required : true)

# ELF64 (64 bits) for kernel
nasm_elf64 = generator(nasm,
	output : ['@BASENAME@.o'],
	arguments : ['-f','elf64','-o','@OUTPUT@','@INPUT@']
)
