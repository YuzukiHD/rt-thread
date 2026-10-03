/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S0 / PCM / TDM controller, one transmit and one receive data line, fed by
 * cyclic DMA rings of fixed periods. The audio PLL and module clock helpers
 * are shared with the codec driver.
 */
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include <rtdevice.h>
#include "dma-sun252i.h"
#include "i2s-sun252i.h"

#define I2S_CTL         0x00
#define I2S_FMT0        0x04
#define I2S_RXFIFO      0x10
#define I2S_FIFOCTL     0x14
#define I2S_INTCTL      0x1c
#define I2S_TXFIFO      0x20
#define I2S_CLKDIV      0x24
#define I2S_TXCNT       0x28
#define I2S_RXCNT       0x2c
#define I2S_CHCFG       0x30
#define I2S_TX0CHSEL    0x34
#define I2S_TX0CHMAP0   0x44
#define I2S_TX0CHMAP1   0x48
#define I2S_RXCHSEL     0x64
#define I2S_RXCHMAP0    0x68

#define BIT(n)          (1u << (n))
#define CTL_GLOBAL_EN   BIT(0)
#define CTL_RXEN        BIT(1)
#define CTL_TXEN        BIT(2)
#define CTL_LOOP        BIT(3)
#define CTL_MODE_SHIFT  4
#define CTL_MODE_MASK   (3u << CTL_MODE_SHIFT)
#define CTL_SDO0_EN     BIT(8)
#define CTL_LRCK_OUT    BIT(17)
#define CTL_BCLK_OUT    BIT(18)
#define FMT0_SLOT_MASK  0x7u
#define FMT0_SAMPLE_SHIFT 4
#define FMT0_SAMPLE_MASK (0x7u << FMT0_SAMPLE_SHIFT)
#define FMT0_LRCK_PERIOD_SHIFT 8
#define FMT0_LRCK_PERIOD_MASK (0x3ffu << FMT0_LRCK_PERIOD_SHIFT)
#define FMT0_BCLK_INV   BIT(7)
#define FMT0_LRCK_INV   BIT(19)
#define FMT0_LRCK_WIDTH BIT(30)
#define FIFOCTL_FRX     BIT(24)
#define FIFOCTL_FTX     BIT(25)
#define FIFOCTL_TXTL_SHIFT 12
#define FIFOCTL_RXTL_SHIFT 4
#define FIFOCTL_TXIM    BIT(2)
#define FIFOCTL_RXOM_MASK 0x3u
#define INTCTL_TXDRQEN  BIT(7)
#define INTCTL_RXDRQEN  BIT(3)
#define CLKDIV_MCLKOUT_EN BIT(8)
#define CHSEL_OFFSET_SHIFT 20
#define CHSEL_CHSEL_SHIFT 16

#define SLOT_WIDTH      32u
#define MCLK_FS         256u

static struct
{
    rt_uint32_t period_bytes, periods;
    rt_uint8_t *tx, *rx;
    volatile rt_uint32_t tx_periods, rx_periods;
    rt_bool_t clk_held, open;
    rt_uint32_t rate;
    rt_ubase_t base;
    struct rt_device *dev;
    struct rt_clk *bus, *mod;
    struct rt_reset_control *rst;
    struct rt_dma_chan *tx_chan, *rx_chan;
} st;

static rt_uint32_t rd(rt_uint32_t o) { return HWREG32(st.base + o); }
static void wr(rt_uint32_t o, rt_uint32_t v) { HWREG32(st.base + o) = v; }
static void upd(rt_uint32_t o, rt_uint32_t m, rt_uint32_t v) { wr(o, (rd(o) & ~m) | (v & m)); }

static int div_code(rt_uint32_t ratio)
{
    static const rt_uint16_t table[] = {1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 176, 192};
    rt_uint32_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i] == ratio)
            return (int)i + 1;
    return -1;
}

static void tx_cb(struct rt_dma_chan *chan, rt_size_t size)
{
    if (size) st.tx_periods++;
}

static void rx_cb(struct rt_dma_chan *chan, rt_size_t size)
{
    if (size) st.rx_periods++;
}

int sun252i_i2s_open(rt_uint32_t rate, rt_uint32_t period_frames, rt_uint32_t periods, rt_bool_t loopback)
{
    rt_uint32_t module, bclk, mclk;
    int bdiv, mdiv;

    if (st.open || !rate || periods > 8u)
        return -1;
    /* the module clock runs at 512 times the rate of the 48 kHz family or 22.5792 MHz */
    module = (rate % 8000u == 0u) ? 24576000u : 22579200u;
    bclk = rate * 2u * SLOT_WIDTH;
    mclk = rate * MCLK_FS;
    bdiv = div_code(module / bclk);
    mdiv = div_code(module / mclk);
    if (bdiv < 0 || mdiv < 0 || module % bclk || module % mclk)
        return -2;

    /* bus gate and reset, then the module clock (the audio PLL follows its rate) */
    rt_reset_control_assert(st.rst);
    rt_clk_prepare_enable(st.bus);
    rt_reset_control_deassert(st.rst);
    if (rt_clk_set_rate(st.mod, module) != RT_EOK)
        return -3;
    rt_clk_prepare_enable(st.mod);
    st.clk_held = RT_TRUE;
    upd(I2S_CTL, CTL_GLOBAL_EN, CTL_GLOBAL_EN);

    /* I2S mode, master, 16 bit samples in 32 bit slots */
    upd(I2S_CTL, CTL_MODE_MASK | CTL_BCLK_OUT | CTL_LRCK_OUT, (1u << CTL_MODE_SHIFT) | CTL_BCLK_OUT | CTL_LRCK_OUT);
    upd(I2S_CTL, CTL_LOOP, loopback ? CTL_LOOP : 0u);
    upd(I2S_FMT0, FMT0_SLOT_MASK | FMT0_LRCK_PERIOD_MASK | FMT0_BCLK_INV | FMT0_LRCK_INV | FMT0_LRCK_WIDTH | FMT0_SAMPLE_MASK,
        7u | ((SLOT_WIDTH - 1u) << FMT0_LRCK_PERIOD_SHIFT) | (3u << FMT0_SAMPLE_SHIFT));
    upd(I2S_CLKDIV, 0xffu | CLKDIV_MCLKOUT_EN, ((rt_uint32_t)bdiv << 4) | (rt_uint32_t)mdiv | CLKDIV_MCLKOUT_EN);
    upd(I2S_TX0CHSEL, 3u << CHSEL_OFFSET_SHIFT, 1u << CHSEL_OFFSET_SHIFT);
    upd(I2S_RXCHSEL, 3u << CHSEL_OFFSET_SHIFT, 1u << CHSEL_OFFSET_SHIFT);
    wr(I2S_TX0CHMAP0, 0xfedcba98u);
    wr(I2S_TX0CHMAP1, 0x76543210u);
    wr(I2S_RXCHMAP0, 0x0f0e0d0cu);
    wr(I2S_RXCHMAP0 + 4, 0x0b0a0908u);
    wr(I2S_RXCHMAP0 + 8, 0x07060504u);
    wr(I2S_RXCHMAP0 + 12, 0x03020100u);

    upd(I2S_FIFOCTL, FIFOCTL_TXIM | (0x7fu << FIFOCTL_TXTL_SHIFT), FIFOCTL_TXIM | (0x40u << FIFOCTL_TXTL_SHIFT));
    upd(I2S_CHCFG, 0xfu, 1u);
    wr(I2S_TX0CHSEL, (rd(I2S_TX0CHSEL) & (3u << CHSEL_OFFSET_SHIFT)) | (1u << CHSEL_CHSEL_SHIFT) | 3u);
    upd(I2S_FIFOCTL, FIFOCTL_RXOM_MASK | (0x7fu << FIFOCTL_RXTL_SHIFT), 1u | (0x1fu << FIFOCTL_RXTL_SHIFT));
    upd(I2S_CHCFG, 0xfu << 4, 1u << 4);
    upd(I2S_RXCHSEL, 0xfu << CHSEL_CHSEL_SHIFT, 1u << CHSEL_CHSEL_SHIFT);

    st.period_bytes = period_frames * 4u;
    st.periods = periods;
    st.tx = rt_malloc_align(st.period_bytes * periods, 64);
    st.rx = rt_malloc_align(st.period_bytes * periods, 64);
    if (!st.tx || !st.rx)
        return -5;
    memset(st.rx, 0, st.period_bytes * periods);
    st.rate = rate;
    st.open = RT_TRUE;
    return 0;
}

rt_uint8_t *sun252i_i2s_tx_buffer(void) { return st.tx; }
rt_uint8_t *sun252i_i2s_rx_buffer(void) { return st.rx; }
rt_uint32_t sun252i_i2s_tx_periods(void) { return st.tx_periods; }
rt_uint32_t sun252i_i2s_rx_periods(void) { return st.rx_periods; }

static rt_err_t ring_prepare(struct rt_dma_chan *chan, enum rt_dma_transfer_direction dir, rt_ubase_t fifo,
                             rt_uint8_t *buf, void (*cb)(struct rt_dma_chan *, rt_size_t))
{
    struct rt_dma_slave_config conf = { .direction = dir };
    struct rt_dma_slave_transfer t = { 0 };
    rt_err_t err;

    conf.src_addr_width = conf.dst_addr_width = RT_DMA_SLAVE_BUSWIDTH_2_BYTES;
    conf.src_maxburst = conf.dst_maxburst = 4;
    if (dir == RT_DMA_MEM_TO_DEV)
    {
        conf.dst_addr = fifo;
        t.src_addr = (rt_ubase_t)buf;
    }
    else
    {
        conf.src_addr = fifo;
        t.dst_addr = (rt_ubase_t)buf;
    }
    t.buffer_len = st.period_bytes * st.periods;
    t.period_len = st.period_bytes;
    chan->callback = cb;
    if ((err = rt_dma_chan_config(chan, &conf)))
        return err;
    if ((err = rt_dma_prep_cyclic(chan, &t)))
        return err;

    return rt_dma_chan_start(chan);
}

int sun252i_i2s_start(void)
{
    if (!st.open)
        return -1;
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, st.tx, st.period_bytes * st.periods);
    st.tx_periods = st.rx_periods = 0;

    /* receive first so that no frame is lost, then the transmitter */
    if (ring_prepare(st.rx_chan, RT_DMA_DEV_TO_MEM, st.base + I2S_RXFIFO, st.rx, rx_cb))
        return -2;
    upd(I2S_FIFOCTL, FIFOCTL_FRX, FIFOCTL_FRX);
    wr(I2S_RXCNT, 0);
    upd(I2S_CTL, CTL_RXEN, CTL_RXEN);
    upd(I2S_INTCTL, INTCTL_RXDRQEN, INTCTL_RXDRQEN);

    if (ring_prepare(st.tx_chan, RT_DMA_MEM_TO_DEV, st.base + I2S_TXFIFO, st.tx, tx_cb))
        return -3;
    upd(I2S_FIFOCTL, FIFOCTL_FTX, FIFOCTL_FTX);
    wr(I2S_TXCNT, 0);
    upd(I2S_INTCTL, INTCTL_TXDRQEN, INTCTL_TXDRQEN);
    upd(I2S_CTL, CTL_SDO0_EN | CTL_TXEN, CTL_SDO0_EN | CTL_TXEN);
    return 0;
}

void sun252i_i2s_stop(void)
{
    upd(I2S_INTCTL, INTCTL_TXDRQEN | INTCTL_RXDRQEN, 0);
    upd(I2S_CTL, CTL_SDO0_EN | CTL_TXEN | CTL_RXEN, 0);
    rt_dma_chan_stop(st.tx_chan);
    rt_dma_chan_stop(st.rx_chan);
}

void sun252i_i2s_close(void)
{
    if (!st.open)
        return;
    sun252i_i2s_stop();
    if (st.clk_held)
    {
        upd(I2S_CTL, CTL_GLOBAL_EN, 0);
        rt_clk_disable_unprepare(st.mod);
        st.clk_held = RT_FALSE;
    }
    rt_free_align(st.tx);
    rt_free_align(st.rx);
    st.tx = st.rx = RT_NULL;
    st.open = RT_FALSE;
}

static rt_err_t i2s_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;

    st.dev = dev;
    st.base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    st.bus = rt_clk_get_by_name(dev, "bus");
    st.mod = rt_clk_get_by_name(dev, "mod");
    st.rst = rt_reset_control_get_by_index(dev, 0);
    st.tx_chan = rt_dma_chan_request(dev, "tx");
    st.rx_chan = rt_dma_chan_request(dev, "rx");
    if (!st.base || rt_is_err_or_null(st.bus) || rt_is_err_or_null(st.mod) || rt_is_err_or_null(st.rst) ||
        rt_is_err_or_null(st.tx_chan) || rt_is_err_or_null(st.rx_chan))
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}

static const struct rt_ofw_node_id i2s_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-i2s" },
    { /* sentinel */ }
};

static struct rt_platform_driver i2s_driver =
{
    .name = "i2s-sun252i",
    .ids = i2s_ofw_ids,
    .probe = i2s_probe,
};
RT_PLATFORM_DRIVER_EXPORT(i2s_driver);
