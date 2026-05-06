ENTRY(_start)

PHDRS {
	text PT_LOAD FLAGS(0x5);
	rodata PT_LOAD FLAGS(0x4);
	data PT_LOAD FLAGS(0x6);
}

SECTIONS {
	. = 0x10000;

	.text : ALIGN(0x1000) {
		*(.text*)
	} :text

	.rodata : ALIGN(0x1000) {
		*(.rodata*)
		*(.srodata*)
		*(.sdata2*)
	} :rodata

	.data : ALIGN(0x1000) {
		*(.data*)
		*(.sdata*)
	} :data

	.bss : ALIGN(0x1000) {
		__bss_start = .;
		*(.bss*)
		*(.sbss*)
		*(COMMON)
		__bss_end = .;
	} :data

	_end = .;
}
