/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device mode glue of the CherryUSB MUSB port: the bus gate and reset of the OTG controller, the
 * PHY, the CPU moves the FIFO data (VEND0 PIO) and the interrupt, all described by the device
 * tree node of the controller.
 */

#include <rtthread.h>
#include <rtdevice.h>

#include "usb-glue-sun252i.h"
#include "usb-phy-sun252i.h"
#include "usb_config.h"
#include "usbd_core.h"

#define VEND0       0x43u
/* VEND0 bit 0: the FIFO bus select, set for both the CPU and the DMA (chosen in the endpoint CSR) */
#define VEND0_BUS   (1u << 0)

/* the DMA engine behind the endpoint FIFOs: one channel per endpoint and direction */
#define DMA_IRQ_EN      0x500u
#define DMA_IRQ_STA     0x504u
#define DMA_CH(ch)      (0x540u + (ch) * 0x10u)
#define DMA_CFG         0x0u
#define DMA_ADDR        0x4u
#define DMA_COUNT       0x8u
#define DMA_RESIDUAL    0xcu
#define DMA_CFG_EN      (1u << 31)
#define DMA_CFG_RX      (1u << 4)
#define DMA_LEN_MASK    0x1ffffu
#define DMA_ALIGN       64u

static struct
{
    rt_ubase_t base;
    int irq;
    struct rt_clk *clk;
    struct rt_reset_control *rst;
    struct sun252i_usb_phy *phy;
    struct rt_ofw_node *np;
} otg;

rt_ubase_t sun252i_usb_otg_base(void)
{
    return otg.base;
}

static void usbd_isr(int vector, void *param)
{
    USBD_IRQHandler(0);
}

#ifdef CONFIG_USB_MUSB_DMA
static rt_ubase_t dma_buf[8];   /* start of the running transfer of each channel */
static rt_uint16_t dma_mps[8];  /* packet size of the running transfer of each channel */

static unsigned int dma_ch(uint8_t ep_idx, bool is_in)
{
    return ep_idx * 2u + (is_in ? 0u : 1u);
}

/* strong overrides of the weak hooks of port/musb/usb_dc_musb.c */
bool usb_musb_dma_start(uint8_t ep_idx, bool is_in, void *buf, uint32_t len, uint16_t mps)
{
    unsigned int ch = dma_ch(ep_idx, is_in);
    rt_ubase_t reg = otg.base + DMA_CH(ch);
    rt_ubase_t start = (rt_ubase_t)buf & ~(DMA_ALIGN - 1u);
    rt_size_t size = ((rt_ubase_t)buf + len - start + DMA_ALIGN - 1u) & ~(DMA_ALIGN - 1u);

    if ((HWREG32(reg + DMA_CFG) & DMA_CFG_EN) || len > DMA_LEN_MASK)
        return false;
    /*
     * The cache of the buffer is written back and dropped line by line: a buffer that shares a
     * line with other data (or a length that ends inside one) would lose the CPU's changes to
     * that data. Such transfers stay with the CPU.
     */
    if (((rt_ubase_t)buf | len) & (DMA_ALIGN - 1u))
        return false;

    /* the buffer goes through the CPU cache: write it out, or drop stale lines before the DMA fills it */
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)start, size);
    if (!is_in)
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)start, size);

    dma_buf[ch] = (rt_ubase_t)buf;
    dma_mps[ch] = mps;
    HWREG32(otg.base + DMA_IRQ_STA) = 1u << ch;
    HWREG32(otg.base + DMA_IRQ_EN) |= 1u << ch;
    HWREG32(reg + DMA_ADDR) = (rt_uint32_t)(rt_ubase_t)buf;
    HWREG32(reg + DMA_COUNT) = len;
    HWREG32(reg + DMA_CFG) = DMA_CFG_EN | ((rt_uint32_t)(mps & 0x7ffu) << 16) | (is_in ? 0u : DMA_CFG_RX) | ep_idx;

    return true;
}

bool usb_musb_dma_done(uint8_t ep_idx, bool is_in, uint32_t *bytes)
{
    unsigned int ch = dma_ch(ep_idx, is_in);
    rt_ubase_t reg = otg.base + DMA_CH(ch);
    rt_uint32_t count;

    if (!(HWREG32(otg.base + DMA_IRQ_STA) & (1u << ch)))
        return false;

    /* a write of 1 clears the channel alone */
    HWREG32(otg.base + DMA_IRQ_STA) = 1u << ch;
    HWREG32(otg.base + DMA_IRQ_EN) &= ~(1u << ch);
    count = HWREG32(reg + DMA_COUNT) & DMA_LEN_MASK;
    *bytes = count - (HWREG32(reg + DMA_RESIDUAL) & DMA_LEN_MASK);
    HWREG32(reg + DMA_CFG) &= ~DMA_CFG_EN;

    if (!is_in)
    {
        rt_ubase_t addr = dma_buf[ch];
        rt_ubase_t start = addr & ~(DMA_ALIGN - 1u);

        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)start,
                             (addr + *bytes - start + DMA_ALIGN - 1u) & ~(DMA_ALIGN - 1u));
    }

    return true;
}

/*
 * Returns once the residual count has not moved for 50 us (a DMA waiting for the memory bus can
 * pause for a few us; one that has nothing left to take stays still), or after 1 ms.
 */
#define MTIME_LO        (*(volatile rt_uint32_t *)0x1400BFF8u)
#define MTIME_PER_US    24u

static rt_uint32_t dma_drain(unsigned int ch, rt_uint32_t count)
{
    rt_ubase_t reg = otg.base + DMA_CH(ch);
    rt_uint32_t begin = MTIME_LO, since = begin, now, res, last = ~0u;

    for (;;)
    {
        res = HWREG32(reg + DMA_RESIDUAL) & DMA_LEN_MASK;
        now = MTIME_LO;
        if (res != last)
        {
            last = res;
            since = now;
        }
        if ((now - since) >= 50u * MTIME_PER_US && (count - res) % dma_mps[ch] == 0)
            break;
        if ((now - begin) >= 1000u * MTIME_PER_US)
            break;
    }

    return res;
}

void usb_musb_dma_settle(uint8_t ep_idx, bool is_in)
{
    unsigned int ch = dma_ch(ep_idx, is_in);

    dma_drain(ch, HWREG32(otg.base + DMA_CH(ch) + DMA_COUNT) & DMA_LEN_MASK);
}

uint32_t usb_musb_dma_abort(uint8_t ep_idx, bool is_in)
{
    unsigned int ch = dma_ch(ep_idx, is_in);
    rt_ubase_t reg = otg.base + DMA_CH(ch);
    rt_uint32_t count = HWREG32(reg + DMA_COUNT) & DMA_LEN_MASK;
    rt_uint32_t res = HWREG32(reg + DMA_RESIDUAL) & DMA_LEN_MASK;
    rt_uint32_t bytes;

    if (!is_in)
        res = dma_drain(ch, count);
    bytes = count - res;

    HWREG32(reg + DMA_CFG) &= ~DMA_CFG_EN;
    HWREG32(otg.base + DMA_IRQ_EN) &= ~(1u << ch);
    HWREG32(otg.base + DMA_IRQ_STA) = 1u << ch;
    if (!is_in && bytes)
    {
        rt_ubase_t start = dma_buf[ch] & ~(DMA_ALIGN - 1u);

        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)start,
                             (dma_buf[ch] + bytes - start + DMA_ALIGN - 1u) & ~(DMA_ALIGN - 1u));
    }

    return bytes;
}
#endif

/* strong override of the weak one of port/musb/usb_dc_musb.c */
void usb_dc_low_level_init(void)
{
    /* the PHY node may probe after this one: look it up when the stack starts */
    otg.phy = sun252i_usb_phy_get(otg.np);
    sun252i_usb_phy_acquire(otg.phy, SUN252I_USB_DEVICE);
    rt_clk_prepare_enable(otg.clk);
    rt_reset_control_deassert(otg.rst);

    HWREG8(otg.base + VEND0) = VEND0_BUS;
#ifdef CONFIG_USB_MUSB_DMA
    HWREG32(otg.base + DMA_IRQ_EN) = 0;
    HWREG32(otg.base + DMA_IRQ_STA) = 0xffffffffu;
#endif

    rt_pic_attach_irq(otg.irq, usbd_isr, RT_NULL, "usbd", 0);
    rt_pic_irq_unmask(otg.irq);
}

void usb_dc_low_level_deinit(void)
{
    rt_pic_irq_mask(otg.irq);
    rt_pic_detach_irq(otg.irq, RT_NULL);
    if (otg.phy)
        sun252i_usb_phy_release(otg.phy, SUN252I_USB_DEVICE);
}

static rt_err_t otg_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;

    otg.base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    otg.irq = rt_dm_dev_get_irq(dev, 0);
    otg.clk = rt_clk_get_by_index(dev, 0);
    otg.rst = rt_reset_control_get_by_index(dev, 0);
    otg.np = dev->ofw_node;
    if (!otg.base || otg.irq < 0 || rt_is_err_or_null(otg.clk) || rt_is_err_or_null(otg.rst))
    {
        otg.base = 0;
        return -RT_ERROR;
    }

    return RT_EOK;
}

static const struct rt_ofw_node_id otg_ofw_ids[] =
{
    { .compatible = "allwinner,sunxi-musb" },
    { /* sentinel */ }
};

static struct rt_platform_driver otg_driver =
{
    .name = "usb-otg-sun252i",
    .ids = otg_ofw_ids,
    .probe = otg_probe,
};
RT_PLATFORM_DRIVER_EXPORT(otg_driver);
