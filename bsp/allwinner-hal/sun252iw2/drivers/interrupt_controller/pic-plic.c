/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform level interrupt controller (PLIC), machine mode context of hart 0.
 * Sources are level triggered; the claim register is read in the trap handler
 * and written back once the handlers ran.
 */
#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>

#define DBG_TAG "pic.plic"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define PLIC_PRIORITY(id)   (0x0000u + (id) * 4u)
#define PLIC_ENABLE(id)     (0x2000u + ((id) / 32u) * 4u)
#define PLIC_THRESHOLD      0x200000u
#define PLIC_CLAIM          0x200004u

struct plic
{
    struct rt_pic parent;
    rt_ubase_t base;
    rt_uint32_t ndev;
};

static struct plic _plic;

rt_inline rt_uint32_t plic_read(struct plic *p, rt_uint32_t off)
{
    return HWREG32(p->base + off);
}

rt_inline void plic_write(struct plic *p, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32(p->base + off) = val;
}

static void plic_irq_unmask(struct rt_pic_irq *pirq)
{
    struct plic *p = pirq->pic->priv_data;

    plic_write(p, PLIC_ENABLE(pirq->hwirq), plic_read(p, PLIC_ENABLE(pirq->hwirq)) | RT_BIT(pirq->hwirq % 32));
}

static void plic_irq_mask(struct rt_pic_irq *pirq)
{
    struct plic *p = pirq->pic->priv_data;

    plic_write(p, PLIC_ENABLE(pirq->hwirq), plic_read(p, PLIC_ENABLE(pirq->hwirq)) & ~RT_BIT(pirq->hwirq % 32));
}

static rt_err_t plic_irq_set_priority(struct rt_pic_irq *pirq, rt_uint32_t priority)
{
    struct plic *p = pirq->pic->priv_data;

    plic_write(p, PLIC_PRIORITY(pirq->hwirq), priority);
    pirq->priority = priority;

    return RT_EOK;
}

static int plic_irq_map(struct rt_pic *pic, int hwirq, rt_uint32_t mode)
{
    struct plic *p = pic->priv_data;
    struct rt_pic_irq *pirq;
    int irq;

    if (hwirq <= 0 || hwirq > p->ndev)
    {
        return -RT_EINVAL;
    }

    pirq = rt_pic_find_irq(pic, hwirq);
    irq = rt_pic_config_irq(pic, hwirq, hwirq);

    if (irq >= 0)
    {
        plic_irq_set_priority(pirq, 1);
    }

    return irq;
}

static rt_err_t plic_irq_parse(struct rt_pic *pic, struct rt_ofw_cell_args *args, struct rt_pic_irq *out_pirq)
{
    if (args->args_count < 1 || args->args[0] == 0 || args->args[0] > _plic.ndev)
    {
        return -RT_EINVAL;
    }

    out_pirq->hwirq = args->args[0];
    out_pirq->mode = RT_IRQ_MODE_LEVEL_HIGH;

    return RT_EOK;
}

static const struct rt_pic_ops plic_ops =
{
    .name = "PLIC",
    .irq_enable = plic_irq_unmask,
    .irq_disable = plic_irq_mask,
    .irq_mask = plic_irq_mask,
    .irq_unmask = plic_irq_unmask,
    .irq_set_priority = plic_irq_set_priority,
    .irq_map = plic_irq_map,
    .irq_parse = plic_irq_parse,
};

static rt_bool_t plic_handler(void *data)
{
    struct plic *p = data;
    rt_uint32_t hwirq;
    rt_bool_t handled = RT_FALSE;

    while ((hwirq = plic_read(p, PLIC_CLAIM)) != 0)
    {
        struct rt_pic_irq *pirq = rt_pic_find_irq(&p->parent, hwirq);

        if (pirq && pirq->irq >= 0)
        {
            rt_pic_handle_isr(pirq);
        }
        else
        {
            LOG_W("unhandled source %u", hwirq);
        }
        plic_write(p, PLIC_CLAIM, hwirq);
        handled = RT_TRUE;
    }

    return handled;
}

static rt_err_t plic_ofw_init(struct rt_ofw_node *np, const struct rt_ofw_node_id *id)
{
    rt_err_t err;
    rt_uint32_t ndev = 0;
    rt_uint64_t reg, reg_size;

    RT_UNUSED(id);

    if (_plic.parent.ops)
    {
        return -RT_EBUSY;
    }
    if ((err = rt_ofw_get_address(np, 0, &reg, &reg_size)))
    {
        return err;
    }
    if (rt_ofw_prop_read_u32(np, "riscv,ndev", &ndev) || !ndev)
    {
        return -RT_EINVAL;
    }

    _plic.base = (rt_ubase_t)reg;
    _plic.ndev = ndev;
    _plic.parent.priv_data = &_plic;
    _plic.parent.ops = &plic_ops;

    /* every source starts masked, the threshold lets all priorities through */
    for (rt_uint32_t i = 0; i <= ndev / 32; i++)
    {
        plic_write(&_plic, 0x2000u + i * 4u, 0);
    }
    plic_write(&_plic, PLIC_THRESHOLD, 0);

    if ((err = rt_pic_linear_irq(&_plic.parent, ndev + 1)))
    {
        _plic.parent.ops = RT_NULL;
        return err;
    }

    rt_pic_add_traps(plic_handler, &_plic);

    rt_ofw_data(np) = &_plic.parent;
    rt_pic_user_extends(&_plic.parent);

    return RT_EOK;
}

static const struct rt_ofw_node_id plic_ofw_ids[] =
{
    { .compatible = "sifive,plic-1.0.0" },
    { .compatible = "riscv,plic0" },
    { /* sentinel */ }
};
RT_PIC_OFW_DECLARE(plic, plic_ofw_ids, plic_ofw_init);
