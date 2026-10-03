/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pin controller and GPIO of the sun252i SoC (one PIO block). The pin number is
 * bank * 32 + pin in the bank: PA0 is 0, PB0 is 32, ... PF31 is 191.
 *
 * Each bank owns a 0x30 byte register page: the function select packs eight pins of
 * four bits at 0x00, the data register is at 0x10, the drive level (four bits, eight pins
 * per register) at 0x14 and the pull select (two bits, sixteen pins per register) at 0x24.
 * The interrupt page of a bank follows all bank pages at 0x200 + bank * 0x20 (trigger
 * type at 0x00, enable at 0x10, status at 0x14); a pin is routed to it with the function 0xe.
 *
 * Pin groups of the device tree carry "pinmux" (cells made with ALLWINNER_PINMUX), the
 * optional "bias-pull-up", "bias-pull-down", "bias-disable" and "drive-strength" (a drive
 * level of the block) properties.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>

#include <dt-bindings/pinctrl/allwinner-pinctrl.h>

#include "dev_pin_dm.h"

#define PIO_BANK_STRIDE     0x30u
#define PIO_CFG_OFF         0x00u
#define PIO_DATA_OFF        0x10u
#define PIO_DRV_OFF         0x14u
#define PIO_PULL_OFF        0x24u
#define PIO_IRQ_PAGE_BASE   0x200u
#define PIO_IRQ_PAGE_STRIDE 0x20u
#define PIO_IRQ_CTL_OFF     0x10u
#define PIO_IRQ_STA_OFF     0x14u
#define PIO_IRQ_MUX_FUNC    0x0eu

#define PIO_BANKS           6u
#define PIO_PINS_PER_BANK   32u
#define PIO_PIN_NR          (PIO_BANKS * PIO_PINS_PER_BANK)

struct pio_irq
{
    void (*hdr)(void *args);
    void *args;
    rt_uint8_t enabled;
};

struct pio
{
    struct rt_device_pin parent;
    void *base;
    struct rt_spinlock lock;
    struct pio_irq irqs[PIO_PIN_NR];
};

#define raw_to_pio(raw) rt_container_of(raw, struct pio, parent)

rt_inline rt_uint32_t pio_read(struct pio *pio, rt_uint32_t off)
{
    return HWREG32((rt_ubase_t)pio->base + off);
}

rt_inline void pio_write(struct pio *pio, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32((rt_ubase_t)pio->base + off) = val;
}

/* a field of @bits bits of pin @pin in a register block starting at @blk of its bank */
static void pio_field(struct pio *pio, rt_base_t pin, rt_uint32_t blk, rt_uint32_t per_reg,
                      rt_uint32_t bits, rt_uint32_t val)
{
    rt_uint32_t num = pin % PIO_PINS_PER_BANK;
    rt_uint32_t off = (pin / PIO_PINS_PER_BANK) * PIO_BANK_STRIDE + blk + (num / per_reg) * 4u;
    rt_uint32_t shift = (num % per_reg) * bits;
    rt_uint32_t mask = (RT_BIT(bits) - 1u) << shift;
    rt_ubase_t level = rt_spin_lock_irqsave(&pio->lock);

    pio_write(pio, off, (pio_read(pio, off) & ~mask) | ((val << shift) & mask));
    rt_spin_unlock_irqrestore(&pio->lock, level);
}

static void pio_set_mux(struct pio *pio, rt_base_t pin, rt_uint32_t mux)
{
    pio_field(pio, pin, PIO_CFG_OFF, 8, 4, mux);
}

static rt_uint32_t pio_get_mux(struct pio *pio, rt_base_t pin)
{
    rt_uint32_t num = pin % PIO_PINS_PER_BANK;
    rt_uint32_t off = (pin / PIO_PINS_PER_BANK) * PIO_BANK_STRIDE + PIO_CFG_OFF + (num / 8u) * 4u;

    return (pio_read(pio, off) >> ((num % 8u) * 4u)) & 0xfu;
}

static void pio_set_pull(struct pio *pio, rt_base_t pin, rt_uint32_t pull)
{
    pio_field(pio, pin, PIO_PULL_OFF, 16, 2, pull);
}

static void pio_set_drive(struct pio *pio, rt_base_t pin, rt_uint32_t level)
{
    pio_field(pio, pin, PIO_DRV_OFF, 8, 4, level);
}

/* an input keeps the route to the interrupt block, only its bias changes */
static void pio_set_input(struct pio *pio, rt_base_t pin, rt_uint32_t pull)
{
    if (pio_get_mux(pio, pin) != PIO_IRQ_MUX_FUNC)
    {
        pio_set_mux(pio, pin, 0);
    }
    pio_set_pull(pio, pin, pull);
}

static rt_uint32_t data_off(rt_base_t pin)
{
    return (pin / PIO_PINS_PER_BANK) * PIO_BANK_STRIDE + PIO_DATA_OFF;
}

static rt_uint32_t irq_page(rt_base_t pin)
{
    return PIO_IRQ_PAGE_BASE + (pin / PIO_PINS_PER_BANK) * PIO_IRQ_PAGE_STRIDE;
}

/* ---- gpio ---------------------------------------------------------------------- */
static void pio_pin_mode(struct rt_device *device, rt_base_t pin, rt_uint8_t mode)
{
    struct pio *pio = raw_to_pio(device);

    if (pin < 0 || pin >= PIO_PIN_NR)
    {
        return;
    }

    switch (mode)
    {
    case PIN_MODE_OUTPUT:
        pio_set_mux(pio, pin, 1);
        break;
    case PIN_MODE_INPUT:
        pio_set_input(pio, pin, 0);
        break;
    case PIN_MODE_INPUT_PULLUP:
        pio_set_input(pio, pin, 1);
        break;
    case PIN_MODE_INPUT_PULLDOWN:
        pio_set_input(pio, pin, 2);
        break;
    case PIN_MODE_OUTPUT_OD:
        pio_set_mux(pio, pin, 1);
        break;
    default:
        break;
    }
}

static void pio_pin_write(struct rt_device *device, rt_base_t pin, rt_uint8_t value)
{
    struct pio *pio = raw_to_pio(device);
    rt_uint32_t off = data_off(pin);
    rt_uint32_t bit = RT_BIT(pin % PIO_PINS_PER_BANK);
    rt_ubase_t level;

    if (pin < 0 || pin >= PIO_PIN_NR)
    {
        return;
    }

    level = rt_spin_lock_irqsave(&pio->lock);
    pio_write(pio, off, value ? (pio_read(pio, off) | bit) : (pio_read(pio, off) & ~bit));
    rt_spin_unlock_irqrestore(&pio->lock, level);
}

static rt_ssize_t pio_pin_read(struct rt_device *device, rt_base_t pin)
{
    struct pio *pio = raw_to_pio(device);

    if (pin < 0 || pin >= PIO_PIN_NR)
    {
        return -RT_EINVAL;
    }

    return (pio_read(pio, data_off(pin)) >> (pin % PIO_PINS_PER_BANK)) & 1u;
}

static rt_err_t pio_pin_irq_mode(struct rt_device *device, rt_base_t pin, rt_uint8_t mode)
{
    /* trigger values of the interrupt page: rising, falling, both edges, high level, low level */
    static const rt_uint8_t trig[] = { 0x0, 0x1, 0x4, 0x2, 0x3 };
    struct pio *pio = raw_to_pio(device);
    rt_uint32_t num, off, shift;
    rt_ubase_t level;

    if (pin < 0 || pin >= PIO_PIN_NR || mode >= RT_ARRAY_SIZE(trig))
    {
        return -RT_EINVAL;
    }

    num = pin % PIO_PINS_PER_BANK;
    off = irq_page(pin) + (num / 8u) * 4u;
    shift = (num % 8u) * 4u;

    level = rt_spin_lock_irqsave(&pio->lock);
    pio_write(pio, off, (pio_read(pio, off) & ~(0xfu << shift)) | ((rt_uint32_t)trig[mode] << shift));
    rt_spin_unlock_irqrestore(&pio->lock, level);

    pio_set_mux(pio, pin, PIO_IRQ_MUX_FUNC);

    return RT_EOK;
}

static rt_err_t pio_pin_attach_irq(struct rt_device *device, rt_base_t pin, rt_uint8_t mode,
                                   void (*hdr)(void *args), void *args)
{
    struct pio *pio = raw_to_pio(device);
    rt_err_t err = pio_pin_irq_mode(device, pin, mode);

    if (!err)
    {
        pio->irqs[pin].hdr = hdr;
        pio->irqs[pin].args = args;
    }

    return err;
}

static rt_err_t pio_pin_detach_irq(struct rt_device *device, rt_base_t pin)
{
    struct pio *pio = raw_to_pio(device);

    if (pin < 0 || pin >= PIO_PIN_NR)
    {
        return -RT_EINVAL;
    }

    pio->irqs[pin].hdr = RT_NULL;
    pio->irqs[pin].args = RT_NULL;
    pio->irqs[pin].enabled = 0;

    return RT_EOK;
}

static rt_err_t pio_pin_irq_enable(struct rt_device *device, rt_base_t pin, rt_uint8_t enabled)
{
    struct pio *pio = raw_to_pio(device);
    rt_uint32_t page, bit;

    if (pin < 0 || pin >= PIO_PIN_NR)
    {
        return -RT_EINVAL;
    }

    page = irq_page(pin);
    bit = RT_BIT(pin % PIO_PINS_PER_BANK);

    if (enabled == PIN_IRQ_ENABLE)
    {
        pio->irqs[pin].enabled = 1;
        pio_write(pio, page + PIO_IRQ_STA_OFF, bit);
        pio_write(pio, page + PIO_IRQ_CTL_OFF, pio_read(pio, page + PIO_IRQ_CTL_OFF) | bit);
    }
    else
    {
        pio_write(pio, page + PIO_IRQ_CTL_OFF, pio_read(pio, page + PIO_IRQ_CTL_OFF) & ~bit);
        pio->irqs[pin].enabled = 0;
    }

    return RT_EOK;
}

/* gpios = <&pio bank number flags> */
static rt_ssize_t pio_pin_parse(struct rt_device *device, struct rt_ofw_cell_args *args, rt_uint32_t *flags)
{
    if (args->args_count < 2 || args->args[0] >= PIO_BANKS || args->args[1] >= PIO_PINS_PER_BANK)
    {
        return -RT_EINVAL;
    }

    if (flags)
    {
        *flags = args->args_count > 2 ? args->args[2] : 0;
    }

    return args->args[0] * PIO_PINS_PER_BANK + args->args[1];
}

static void pio_bank_isr(int irq, void *param)
{
    struct pio *pio = param;
    rt_uint32_t bank, pending, i;

    for (bank = 0; bank < PIO_BANKS; bank++)
    {
        rt_uint32_t page = PIO_IRQ_PAGE_BASE + bank * PIO_IRQ_PAGE_STRIDE;

        pending = pio_read(pio, page + PIO_IRQ_STA_OFF);
        for (i = 0; i < PIO_PINS_PER_BANK && pending; i++)
        {
            struct pio_irq *pi = &pio->irqs[bank * PIO_PINS_PER_BANK + i];

            if (!(pending & RT_BIT(i)))
            {
                continue;
            }
            pending &= ~RT_BIT(i);
            pio_write(pio, page + PIO_IRQ_STA_OFF, RT_BIT(i));
            if (pi->enabled && pi->hdr)
            {
                pi->hdr(pi->args);
            }
        }
    }
}

/* ---- pin controller: pin groups of the device tree ------------------------------- */
static rt_err_t pio_confs_apply(struct rt_device *device, void *fw_conf_np)
{
    struct pio *pio = raw_to_pio(device);
    struct rt_ofw_node *np = fw_conf_np;
    struct rt_ofw_prop *prop;
    const fdt32_t *cell;
    rt_uint32_t value, level;
    rt_int32_t pull = -1, drive = -1;

    if (rt_ofw_prop_read_bool(np, "bias-pull-up"))
    {
        pull = 1;
    }
    else if (rt_ofw_prop_read_bool(np, "bias-pull-down"))
    {
        pull = 2;
    }
    else if (rt_ofw_prop_read_bool(np, "bias-disable"))
    {
        pull = 0;
    }

    if (!rt_ofw_prop_read_u32(np, "drive-strength", &level))
    {
        drive = level;
    }

    rt_ofw_foreach_prop_u32(np, "pinmux", prop, cell, value)
    {
        rt_base_t pin = ALLWINNER_PINMUX_PIN(value);

        if (pin >= PIO_PIN_NR)
        {
            return -RT_EINVAL;
        }
        if (pull >= 0)
        {
            pio_set_pull(pio, pin, pull);
        }
        if (drive >= 0)
        {
            pio_set_drive(pio, pin, drive);
        }
        pio_set_mux(pio, pin, ALLWINNER_PINMUX_MUXSEL(value));
    }

    return RT_EOK;
}

static rt_err_t pio_gpio_request(struct rt_device *device, rt_base_t gpio, rt_uint32_t flags)
{
    struct pio *pio = raw_to_pio(device);

    RT_UNUSED(flags);
    pio_set_mux(pio, gpio, 0);

    return RT_EOK;
}

static const struct rt_pin_ops pio_ops =
{
    .pin_mode = pio_pin_mode,
    .pin_write = pio_pin_write,
    .pin_read = pio_pin_read,
    .pin_attach_irq = pio_pin_attach_irq,
    .pin_detach_irq = pio_pin_detach_irq,
    .pin_irq_enable = pio_pin_irq_enable,
    .pin_irq_mode = pio_pin_irq_mode,
    .pin_parse = pio_pin_parse,
    .pin_ctrl_confs_apply = pio_confs_apply,
    .pin_ctrl_gpio_request = pio_gpio_request,
};

static rt_err_t pio_probe(struct rt_platform_device *pdev)
{
    rt_err_t err;
    struct rt_device *dev = &pdev->parent;
    struct pio *pio = rt_calloc(1, sizeof(*pio));

    if (!pio)
    {
        return -RT_ENOMEM;
    }

    pio->base = rt_dm_dev_iomap(dev, 0);
    if (!pio->base)
    {
        err = -RT_EIO;
        goto _fail;
    }

    rt_spin_lock_init(&pio->lock);

    pio->parent.ops = &pio_ops;
    rt_dm_dev_bind_fwdata(dev, RT_NULL, &pio->parent);

    if ((err = pin_api_init(&pio->parent, PIO_PIN_NR)))
    {
        goto _fail;
    }

    /* one interrupt per bank */
    for (int bank = 0; bank < PIO_BANKS; bank++)
    {
        int irq = rt_dm_dev_get_irq(dev, bank);

        if (irq < 0)
        {
            continue;
        }
        rt_hw_interrupt_install(irq, pio_bank_isr, pio, "pio");
        rt_hw_interrupt_umask(irq);
    }

    return RT_EOK;

_fail:
    rt_free(pio);

    return err;
}

static const struct rt_ofw_node_id pio_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-pio" },
    { /* sentinel */ }
};

static struct rt_platform_driver pio_driver =
{
    .name = "pinctrl-sun252i",
    .ids = pio_ofw_ids,
    .probe = pio_probe,
};

static int pio_drv_register(void)
{
    rt_platform_driver_register(&pio_driver);

    return 0;
}
INIT_SUBSYS_EXPORT(pio_drv_register);
