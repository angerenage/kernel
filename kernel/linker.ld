ENTRY(_start)

MEMORY {
	virt : ORIGIN = 0xffffffff80000000, LENGTH = 512M
	phys : ORIGIN = 0x00100000, LENGTH = 512M
}

PHDRS {
	text PT_LOAD FLAGS(0x5);
	rodata PT_LOAD FLAGS(0x4);
	data PT_LOAD FLAGS(0x6);
}

SECTIONS {
	. = 0xffffffff80000000;

	.limine_requests_start : ALIGN(16) {
		KEEP(*(.limine_requests_start_marker))
	} > virt AT> phys :text

	.limine_requests : ALIGN(16) {
		KEEP(*(.limine_requests))
	} > virt AT> phys :text

	.limine_requests_end : ALIGN(16) {
		KEEP(*(.limine_requests_end_marker))
	} > virt AT> phys :text

	.text : ALIGN(16) {
		*(.text*)
	} > virt AT> phys :text

	.rodata : ALIGN(0x1000) {
		__start_acpi_excluded_tables = .;
		KEEP(*(acpi_excluded_tables))
		__stop_acpi_excluded_tables = .;
		ASSERT(((__stop_acpi_excluded_tables - __start_acpi_excluded_tables) % 4) == 0,
		       "ACPI exclusion records must contain exactly four bytes")
		*(.rodata*)
		*(.srodata*)
		*(.sdata2*)
	} > virt AT> phys :rodata

	.data : ALIGN(0x1000) {
		*(.data*)
		*(.sdata*)
	} > virt AT> phys :data

	.bss : ALIGN(0x1000) {
		__bss_start = .;
		*(.bss*)
		*(.sbss*)
		*(COMMON)
		__bss_end = .;
	} > virt AT> phys :data
	_end = .;
}
