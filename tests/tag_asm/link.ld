ENTRY(seal_tag)
SECTIONS { . = 0x1000; .text : { *(.text*) *(.rodata*) } }
