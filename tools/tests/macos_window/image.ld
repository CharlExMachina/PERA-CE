ENTRY(guest_main)
SECTIONS
{
	. = 0x88000000;
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(0x4000);
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.*) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
