ENTRY(seal_encode)
SECTIONS { . = 0x1000; .text : { *(.text*) *(.rodata*) } }
