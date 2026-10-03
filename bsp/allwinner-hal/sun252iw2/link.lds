/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The image is loaded at the start of the PSRAM by the first-stage loader.
 */
OUTPUT_ARCH("riscv")

MEMORY
{
    PSRAM : ORIGIN = 0x40000000, LENGTH = 14M
}

__STACKSIZE__ = 4096;

ENTRY(_start)
SECTIONS
{
    . = 0x40000000 ;

    .start :
    {
        *(.start)
    } > PSRAM

    .text :
    {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.srodata)
        *(.srodata.*)
        *(.init_array)
        *(.fini_array)

        . = ALIGN(4);
        __fsymtab_start = .;
        KEEP(*(FSymTab))
        __fsymtab_end = .;

        . = ALIGN(4);
        __vsymtab_start = .;
        KEEP(*(VSymTab))
        __vsymtab_end = .;

        /* device tree stubs of the device model: interrupt controllers and other early drivers */
        . = ALIGN(4);
        KEEP(*(SORT(.rt_ofw_data.*)))

        /* class drivers of the USB host stack */
        . = ALIGN(4);
        __usbh_class_info_start__ = .;
        KEEP(*(.usbh_class_info))
        __usbh_class_info_end__ = .;

        . = ALIGN(4);
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;
    } > PSRAM

    . = ALIGN(8);
    __etext = .;

    .data : AT (__etext)
    {
        __data_start = .;
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.sdata.*)
        . = ALIGN(8);
        __data_end = .;
    } > PSRAM

    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.sbss)
        *(.sbss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > PSRAM

    . = ALIGN(16);
    __stack_start__ = .;
    . = . + __STACKSIZE__;
    . = ALIGN(16);
    __stack_end__ = .;
    __rt_rvstack = .;

    end = .;
}
