/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Interrupt registration for the archive: one line, the video engine's */

#include <stdint.h>
#include <rthw.h>
#include "glue.h"

typedef int (*glue_irq_handler_t)(void *data);

static glue_irq_handler_t ve_handler;
static void *ve_handler_data;
static int ve_irq_connected;

static void ve_isr(int vector, void *arg)
{
    (void)vector;
    (void)arg;

    if (ve_handler != RT_NULL)
        (void)ve_handler(ve_handler_data);
}

int32_t hal_request_irq(int32_t irq, glue_irq_handler_t handler, const char *name, void *data)
{
    (void)name;

    /* The archive names the line by its own numbering; there is only one. */
    if (!ve_irq_connected)
    {
        rt_pic_attach_irq(ve_glue_irq(), ve_isr, RT_NULL, "ve", 0);
        ve_irq_connected = 1;
    }
    ve_handler = handler;
    ve_handler_data = data;

    return 0;
}

void hal_free_irq(int32_t irq)
{
    (void)irq;

    rt_pic_irq_mask(ve_glue_irq());
    ve_handler = RT_NULL;
    ve_handler_data = RT_NULL;
}

int hal_enable_irq(int32_t irq)
{
    (void)irq;

    rt_pic_irq_unmask(ve_glue_irq());
    return 0;
}

void hal_disable_irq(int32_t irq)
{
    (void)irq;

    rt_pic_irq_mask(ve_glue_irq());
}
