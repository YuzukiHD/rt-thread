/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Machine trap dispatch. The port of libcpu/risc-v/common calls handle_trap():
 * exceptions are reported on the raw console and stop the CPU, the machine
 * timer and software sources go to the kernel, the external source is handed to
 * the interrupt controller driver.
 */
#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include "riscv-ops.h"
#include "drv_systick.h"

extern rt_isr_handler_t SW_handler;
static void system_irq_handler(rt_uint32_t mcause);
void rt_tick_interrupt_clear(void);

/* the trap entry of libcpu/risc-v/common calls this */
static void raw_puts(const char *str)
{
    for (; *str; str++)
    {
        while ((*(volatile rt_uint32_t *)(0x02500C00u + 0x14u) & 0x20u) == 0u)
            ;
        *(volatile rt_uint32_t *)0x02500C00u = *str;
    }
}

static void raw_hex(rt_uint32_t v)
{
    char b[11] = "0x";
    int i;
    for (i = 0; i < 8; i++)
        b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xf];
    b[10] = 0;
    raw_puts(b);
}

void handle_trap(rt_uint32_t mcause, rt_uint32_t epc, rt_uint32_t sp)
{
    if ((mcause & 0x80000000u) == 0u)
    {
        raw_puts("\r\nEXC cause=");
        raw_hex(mcause);
        raw_puts(" epc=");
        raw_hex(epc);
        raw_puts(" tval=");
        raw_hex(read_csr(mtval));
        raw_puts(" ra=");
        raw_hex(((rt_uint32_t *)sp)[1]);
        raw_puts(" s0=");
        raw_hex(((rt_uint32_t *)sp)[8]);
        for (;;)
            ;
    }
    system_irq_handler(mcause);
}

static void system_irq_handler(rt_uint32_t mcause)
{
    rt_uint32_t cause = mcause & 0x1Fu;

    if (cause == 11u)               /* machine external: the interrupt controller */
    {
        rt_pic_do_traps();
    }
    else if (cause == 7u)           /* machine timer */
    {
        drv_systick_isr();
    }
    else if (cause == 3u)           /* machine software: thread switch */
    {
        SW_handler(0, RT_NULL);
    }
    else
    {
        rt_kprintf("unhandled machine cause 0x%08x\n", mcause);
    }
}
