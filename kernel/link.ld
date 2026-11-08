/* MC/PS kernel: 16 KiB of ROM at 04000000h (also seen at 0 until the kernel remaps RAM there). */
MEMORY { ROM (rx) : ORIGIN = 0x04000000, LENGTH = 16K }
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > ROM
  /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.exidx*) *(.comment) }
}
