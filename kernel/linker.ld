OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0xffffffff80000000;

  .text : ALIGN(0x1000)
  {
    *(.text .text.*)
  } :text

  .rodata : ALIGN(0x1000)
  {
    *(.rodata .rodata.*)
  } :rodata

  .data : ALIGN(0x1000)
  {
    . = ALIGN(8);
    KEEP(*(.limine_requests_start))
    KEEP(*(.limine_requests))
    KEEP(*(.limine_requests_end))
    *(.data .data.*)
  } :data

  .bss : ALIGN(0x1000)
  {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  } :data
}
