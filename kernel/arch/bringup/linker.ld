ENTRY(port_main)
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0xffffffff80000000;
    .text : { *(.text .text.*) } :text
    .rodata : ALIGN(4096) { *(.rodata .rodata.* .srodata .srodata.*) } :rodata
    .data : ALIGN(4096) {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        *(.data .data.* .sdata .sdata.*)
    } :data
    .bss : ALIGN(4096) { *(.bss .bss.* .sbss .sbss.* COMMON) } :data
}
