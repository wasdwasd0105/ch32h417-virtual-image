MEMORY { RAM (rwx) : ORIGIN = 0x20140000, LENGTH = 0x20000 }
ENTRY(_start)
SECTIONS {
  .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) . = ALIGN(4); } > RAM
  .data : { *(.data*) *(.sdata*) . = ALIGN(4); } > RAM
  .bss  : { *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
