/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI controller of the sun252i SoC (master, polled FIFO transfers, software owned chip select).
 * The bus is described by its device tree node: registers, bus and module clock, reset, pins.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>

#define SPI_GC          0x04u
#define SPI_TC          0x08u
#define SPI_INT_CTL     0x10u
#define SPI_INT_STA     0x14u
#define SPI_FIFO_CTL    0x18u
#define SPI_FIFO_STA    0x1cu
#define SPI_CLK_CTL     0x24u
#define SPI_BURST_CNT   0x30u
#define SPI_TX_CNT      0x34u
#define SPI_BCC         0x38u
#define SPI_TXDATA      0x200u
#define SPI_RXDATA      0x300u

#define GC_EN           0x01u
#define GC_MODE         0x02u
#define GC_MODE_SEL     0x04u
#define GC_TP_EN        0x80u
#define GC_SRST         0x80000000u

#define TC_CPHA         0x01u
#define TC_CPOL         0x02u
#define TC_SPOL         0x04u
#define TC_SS_OWNER     0x40u
#define TC_SS_LEVEL     0x80u
#define TC_DHB          0x100u
#define TC_DDB          0x200u
#define TC_SDM          0x2000u
#define TC_FBS          0x1000u
#define TC_XCH          0x80000000u

#define INT_STA_ERR     0x0700u
#define INT_STA_TC      0x1000u

#define FIFO_TX_RST     0x80000000u
#define FIFO_RX_RST     0x8000u
#define FIFO_TX_CNT     0x00ff0000u
#define FIFO_RX_CNT     0x000000ffu

#define BCC_STC         0x00ffffffu

#define SPI_MODULE_HZ   24000000u
#define SPI_FIFO_DEPTH  64u
#define SPI_TIMEOUT_US  100000u
#define SPI_MAX_BURST   0xffffffu


struct sun_spi
{
    struct rt_spi_bus bus;
    rt_ubase_t base;
    struct rt_clk *mod;
    rt_uint32_t freq;
    char name[RT_NAME_MAX];
};

static rt_uint32_t spi_read(const struct sun_spi *s, rt_uint32_t off)
{
    return HWREG32(s->base + off);
}

static void spi_write(const struct sun_spi *s, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32(s->base + off) = val;
}

static rt_err_t spi_set_clock(struct sun_spi *s, rt_uint32_t frequency)
{
    rt_uint32_t n = 0u;

    if (frequency < 3000u)
        return -RT_EINVAL;

    /* the module clock divider is 24 MHz / 2^N, sourced from the crystal */
    while (n < 15u && (SPI_MODULE_HZ >> n) > frequency)
        n++;
    rt_clk_set_rate(s->mod, SPI_MODULE_HZ >> n);
    spi_write(s, SPI_CLK_CTL, (spi_read(s, SPI_CLK_CTL) & ~0x1fffu) | (n << 8));
    s->freq = frequency;
    return RT_EOK;
}

static rt_err_t spi_reset_fifo(struct sun_spi *s)
{
    rt_uint32_t reg = spi_read(s, SPI_FIFO_CTL);
    rt_uint32_t n;

    reg = (reg & ~(0x00ff0000u | 0x000000ffu)) | FIFO_TX_RST | FIFO_RX_RST |
          (32u << 16) | 32u;
    spi_write(s, SPI_FIFO_CTL, reg);

    for (n = 0u; n < SPI_TIMEOUT_US; n++)
    {
        if ((spi_read(s, SPI_FIFO_CTL) & (FIFO_TX_RST | FIFO_RX_RST)) == 0u)
            return RT_EOK;
        rt_hw_us_delay(1u);
    }
    return -RT_ETIMEOUT;
}

static rt_err_t spi_configure(struct rt_spi_device *device,
        struct rt_spi_configuration *cfg)
{
    struct rt_spi_bus *bus = device->bus;
    struct sun_spi *s = (struct sun_spi *)bus->parent.user_data;
    rt_uint32_t gc, tc;
    rt_err_t err;

    if (cfg->mode & RT_SPI_CPHA) { /* nothing special, flag used below */ }
    if (cfg->data_width != 8u)
        return -RT_EINVAL;

    err = spi_set_clock(s, cfg->max_hz);
    if (err != RT_EOK)
        return err;

    gc = spi_read(s, SPI_GC);
    gc |= GC_EN | GC_MODE | GC_TP_EN;
    gc &= ~GC_MODE_SEL;
    spi_write(s, SPI_GC, gc);

    tc = spi_read(s, SPI_TC) & ~(TC_CPHA | TC_CPOL | TC_SPOL | TC_FBS | TC_DHB);
    if (cfg->mode & RT_SPI_CPHA)
        tc |= TC_CPHA;
    if (cfg->mode & RT_SPI_CPOL)
        tc |= TC_CPOL;
    if (cfg->mode & RT_SPI_MSB)
        { /* MSB first is the default */ }
    else
        tc |= TC_FBS;
    /* software owned select: idle high, taken low by the first message of a transfer */
    tc |= TC_SS_OWNER | TC_SS_LEVEL | TC_SDM | TC_DDB | TC_SPOL;
    spi_write(s, SPI_TC, tc);

    spi_write(s, SPI_INT_CTL, 0u);
    spi_write(s, SPI_INT_STA, 0xffffffffu);
    return spi_reset_fifo(s);
}

static rt_ssize_t spi_xfer(struct rt_spi_device *device, struct rt_spi_message *msg)
{
    struct rt_spi_bus *bus = device->bus;
    struct sun_spi *s = (struct sun_spi *)bus->parent.user_data;
    const rt_uint8_t *tx = (const rt_uint8_t *)msg->send_buf;
    rt_uint8_t *rx = (rt_uint8_t *)msg->recv_buf;
    rt_size_t len = msg->length;
    rt_size_t tx_done = 0u, rx_done = 0u;
    rt_uint32_t waited = 0u;
    rt_bool_t started = RT_FALSE;

    if (msg->cs_take)
        spi_write(s, SPI_TC, spi_read(s, SPI_TC) & ~TC_SS_LEVEL);
    if (len == 0u)
    {
        if (msg->cs_release)
            spi_write(s, SPI_TC, spi_read(s, SPI_TC) | TC_SS_LEVEL);
        return 0u;
    }
    if (len > SPI_MAX_BURST)
        return -RT_ERROR;

    /* send only: the bytes clocked in meanwhile are discarded by the core (DHB) */
    spi_write(s, SPI_TC, rx ? (spi_read(s, SPI_TC) & ~TC_DHB) : (spi_read(s, SPI_TC) | TC_DHB));
    spi_write(s, SPI_BURST_CNT, (rt_uint32_t)len);
    spi_write(s, SPI_TX_CNT, (rt_uint32_t)len);
    spi_write(s, SPI_BCC, (spi_read(s, SPI_BCC) & ~BCC_STC) | (rt_uint32_t)len);

    while (waited++ < SPI_TIMEOUT_US)
    {
        while (tx_done < len &&
               ((spi_read(s, SPI_FIFO_STA) & FIFO_TX_CNT) >> 16u) < SPI_FIFO_DEPTH)
        {
            *(volatile rt_uint8_t *)(s->base + SPI_TXDATA) = tx ? tx[tx_done] : 0xffu;
            tx_done++;
        }

        if (!started)
        {
            spi_write(s, SPI_TC, spi_read(s, SPI_TC) | TC_XCH);
            started = RT_TRUE;
        }

        while (rx != RT_NULL && (spi_read(s, SPI_FIFO_STA) & FIFO_RX_CNT) != 0u && rx_done < len)
        {
            rx[rx_done++] = *(volatile rt_uint8_t *)(s->base + SPI_RXDATA);
        }

        if ((spi_read(s, SPI_INT_STA) & INT_STA_ERR) != 0u)
        {
            spi_write(s, SPI_INT_STA, 0xffffffffu);
            return -RT_ERROR;
        }
        if (tx_done == len && (rx == RT_NULL || rx_done == len) &&
            (spi_read(s, SPI_INT_STA) & INT_STA_TC) != 0u)
        {
            spi_write(s, SPI_INT_STA, 0xffffffffu);
            if (msg->cs_release)
                spi_write(s, SPI_TC, spi_read(s, SPI_TC) | TC_SS_LEVEL);
            return (rt_ssize_t)len;
        }
        rt_hw_us_delay(1u);
    }

    spi_write(s, SPI_INT_STA, 0xffffffffu);
    spi_write(s, SPI_TC, spi_read(s, SPI_TC) | TC_SS_LEVEL);
    return -RT_ETIMEOUT;
}

static const struct rt_spi_ops spi_ops =
{
    spi_configure,
    spi_xfer,
};

static rt_err_t spi_probe(struct rt_platform_device *pdev)
{
    rt_err_t err;
    struct rt_device *dev = &pdev->parent;
    struct sun_spi *s = rt_calloc(1, sizeof(*s));
    struct rt_clk *ahb;
    struct rt_reset_control *rst;

    if (!s)
    {
        return -RT_ENOMEM;
    }

    s->base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    if (!s->base)
    {
        err = -RT_EIO;
        goto _fail;
    }

    ahb = rt_clk_get_by_name(dev, "ahb");
    s->mod = rt_clk_get_by_name(dev, "mod");
    rst = rt_reset_control_get_by_index(dev, 0);
    if (rt_is_err_or_null(ahb) || rt_is_err_or_null(s->mod) || rt_is_err_or_null(rst))
    {
        err = -RT_ERROR;
        goto _fail;
    }
    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(ahb);
    rt_clk_prepare_enable(s->mod);

    rt_snprintf(s->name, sizeof(s->name), "spi%d", pdev->dev_id < 0 ? 0 : pdev->dev_id);
    s->bus.parent.user_data = s;
    s->bus.parent.ofw_node = dev->ofw_node;
    dev->user_data = s;

    if ((err = rt_spi_bus_register(&s->bus, s->name, &spi_ops)))
    {
        goto _fail;
    }

    return RT_EOK;

_fail:
    rt_free(s);

    return err;
}

static const struct rt_ofw_node_id spi_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-spi" },
    { /* sentinel */ }
};

static struct rt_platform_driver spi_driver =
{
    .name = "spi-sun252i",
    .ids = spi_ofw_ids,
    .probe = spi_probe,
};
RT_PLATFORM_DRIVER_EXPORT(spi_driver);
