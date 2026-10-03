/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MIPI DBI type C (SPI 4 wire) transmitter on the SPI1 block: command and
 * data bytes with the D/CX line driven by the controller, FIFO transfers for
 * short messages and DMA for the long ones (frame data).
 *
 * The pins of the port (PD0..PD5) are shared with the RGB panel interface: they
 * are only taken over by sun252i_dbi_attach_pins(), a board uses either the RGB
 * display or a DBI display.
 */
#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include "mipi-dbi-sun252i.h"

#define DBI_MODULE_HZ_DEFAULT   24000000u
#define DBI_CLOCK_HZ_DEFAULT    5000000u
#define DBI_TIMEOUT_US          100000u

#define SPI_GCR             0x04u
#define SPI_TC              0x08u
#define SPI_INT_STA         0x14u
#define SPI_FIFO_CTL        0x18u
#define SPI_FIFO_STA        0x1cu
#define SPI_CLK_CTL         0x24u
#define SPI_BURST_CNT       0x30u
#define SPI_TX_CNT          0x34u
#define SPI_BCC             0x38u
#define DBI_CTRL0           0x100u
#define DBI_CTRL1           0x104u
#define DBI_CTRL2           0x108u
#define DBI_SIZE            0x110u
#define DBI_INT             0x120u
#define DBI_TXFIFO          0x200u

#define BIT(n)              (1u << (n))
#define SPI_GCR_ENABLE      BIT(0)
#define SPI_GCR_MASTER      BIT(1)
#define SPI_GCR_DBI_MODE    BIT(3)
#define SPI_GCR_DBI_ENABLE  BIT(4)
#define SPI_GCR_SOFT_RESET  BIT(31)
#define SPI_TC_SS_LEVEL     BIT(7)
#define SPI_TC_SS_OWNER     BIT(6)
#define SPI_TC_SPOL         BIT(2)
#define SPI_TC_DHB          BIT(8)
#define SPI_TC_SDM          BIT(13)
#define SPI_TC_XCH          BIT(31)
#define SPI_FIFO_TX_RESET   BIT(31)
#define SPI_FIFO_TX_DRQEN   BIT(24)
#define SPI_FIFO_TX_COUNT_SHIFT 16
#define SPI_FIFO_TX_COUNT_MASK  (0xffu << SPI_FIFO_TX_COUNT_SHIFT)
#define SPI_FIFO_DEPTH      64u
#define DBI_CTRL0_INTERFACE_SHIFT 8
#define DBI_CTRL0_INTERFACE (7u << DBI_CTRL0_INTERFACE_SHIFT)
#define DBI_CTRL0_FORMAT    (7u << 12)
#define DBI_CTRL1_DCX_DATA  BIT(22)
#define DBI_CTRL2_HRDY_BYPASS BIT(31)
#define DBI_CTRL2_DCX_PIN   BIT(5)
#define DBI_CTRL2_DMA_ENABLE BIT(15)
#define DBI_INT_STATUS_MASK (0x7fu << 8)
#define DBI_MODULE_HZ       24000000u
#define DBI_MAX_BURST       0x00ffffffu
#define DBI_INTERFACE_D2LI  4u

static struct rt_mutex dbi_lock;
static struct rt_semaphore dma_sem;
static volatile int dma_status;
static rt_bool_t dbi_ready;
static rt_ubase_t dbi_base;
static struct rt_clk *dbi_mod;
static struct rt_dma_chan *dbi_dma;
static rt_base_t pin_cs = -1, pin_reset = -1;
static rt_uint32_t dbi_clock_hz = DBI_CLOCK_HZ_DEFAULT;

static rt_uint32_t rd(rt_uint32_t o) { return HWREG32(dbi_base + o); }
static void wr(rt_uint32_t o, rt_uint32_t v) { HWREG32(dbi_base + o) = v; }

static int wait_fifo_space(void)
{
    rt_uint32_t i;

    for (i = 0; i < DBI_TIMEOUT_US; i++)
    {
        if (((rd(SPI_FIFO_STA) & SPI_FIFO_TX_COUNT_MASK) >> SPI_FIFO_TX_COUNT_SHIFT) < SPI_FIFO_DEPTH)
            return 0;
        rt_hw_us_delay(1u);
    }
    return -RT_ETIMEOUT;
}

static int wait_complete(void)
{
    rt_uint32_t i;

    for (i = 0; i < DBI_TIMEOUT_US; i++)
    {
        if (((rd(SPI_FIFO_STA) & SPI_FIFO_TX_COUNT_MASK) >> SPI_FIFO_TX_COUNT_SHIFT) == 0u && rd(SPI_BURST_CNT) == 0u)
        {
            wr(SPI_INT_STA, 0xffffffffu);
            wr(DBI_INT, DBI_INT_STATUS_MASK);
            return 0;
        }
        rt_hw_us_delay(1u);
    }
    wr(SPI_INT_STA, 0xffffffffu);
    wr(DBI_INT, DBI_INT_STATUS_MASK);
    return -RT_ETIMEOUT;
}

static int set_clock(void)
{
    rt_uint32_t divider = 0u, reg;

    /* the bit clock is the 24 MHz reference divided by a power of two, the first one not above the request */
    while (divider < 15u && (DBI_MODULE_HZ_DEFAULT >> divider) > dbi_clock_hz)
        divider++;
    rt_clk_set_rate(dbi_mod, DBI_MODULE_HZ_DEFAULT >> divider);
    /* the controller runs at the module clock: its own divider stays off, or the bit clock drops further */
    reg = rd(SPI_CLK_CTL);
    reg &= ~(0xfu << 8 | BIT(12) | 0xffu);
    wr(SPI_CLK_CTL, reg);
    return 0;
}

static int reset_fifo(void)
{
    rt_uint32_t reg = rd(SPI_FIFO_CTL), i;

    reg |= SPI_FIFO_TX_RESET;
    reg &= ~(0xffu << 16 | 0xffu);
    reg |= (32u << 16) | 32u;
    wr(SPI_FIFO_CTL, reg);
    for (i = 0; i < DBI_TIMEOUT_US; i++)
    {
        if ((rd(SPI_FIFO_CTL) & SPI_FIFO_TX_RESET) == 0u)
            return 0;
        rt_hw_us_delay(1u);
    }
    return -RT_ETIMEOUT;
}

static void set_cs(rt_bool_t active)
{
    if (pin_cs >= 0)
        rt_pin_write(pin_cs, active ? PIN_LOW : PIN_HIGH);
}

static int transfer_chunk(const rt_uint8_t *buf, rt_size_t len)
{
    rt_uint32_t reg;
    rt_size_t i;
    int ret;

    if (!buf || len == 0u || len > DBI_MAX_BURST)
        return -RT_EINVAL;
    ret = reset_fifo();
    if (ret)
        return ret;
    set_cs(RT_TRUE);

    /* DBI and SPI counters count bytes in the current transaction */
    wr(SPI_BURST_CNT, len);
    wr(SPI_TX_CNT, len);
    wr(SPI_BCC, len & 0x00ffffffu);
    wr(SPI_INT_STA, 0xffffffffu);
    wr(DBI_INT, DBI_INT_STATUS_MASK);
    wr(SPI_TC, rd(SPI_TC) & ~(SPI_TC_SS_LEVEL | SPI_TC_XCH));

    for (i = 0; i < (len < SPI_FIFO_DEPTH ? len : SPI_FIFO_DEPTH); i++)
    {
        ret = wait_fifo_space();
        if (ret)
            goto out_cs;
        *(volatile rt_uint8_t *)(dbi_base + DBI_TXFIFO) = buf[i];
    }
    wr(SPI_TC, rd(SPI_TC) | SPI_TC_XCH);
    for (i = SPI_FIFO_DEPTH; i < len; i++)
    {
        ret = wait_fifo_space();
        if (ret)
            goto out_cs;
        *(volatile rt_uint8_t *)(dbi_base + DBI_TXFIFO) = buf[i];
    }
    ret = wait_complete();

out_cs:
    wr(SPI_TC, rd(SPI_TC) | SPI_TC_SS_LEVEL);
    set_cs(RT_FALSE);
    return ret;
}

static void dma_cb(struct rt_dma_chan *chan, rt_size_t size)
{
    dma_status = size ? 0 : -RT_EIO;
    rt_sem_release(&dma_sem);
}

static int transfer_dma(const rt_uint8_t *buf, rt_size_t len)
{
    struct rt_dma_slave_config conf = { .direction = RT_DMA_MEM_TO_DEV };
    struct rt_dma_slave_transfer t = { 0 };
    int ret;

    if (len == 0u || len > DBI_MAX_BURST)
        return -RT_EINVAL;
    ret = reset_fifo();
    if (ret)
        return ret;
    wr(SPI_BURST_CNT, len);
    wr(SPI_TX_CNT, len);
    wr(SPI_BCC, len & 0x00ffffffu);
    wr(SPI_INT_STA, 0xffffffffu);
    wr(DBI_INT, DBI_INT_STATUS_MASK);
    wr(SPI_TC, rd(SPI_TC) & ~SPI_TC_SS_LEVEL);
    set_cs(RT_TRUE);

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)buf, len);
    while (rt_sem_trytake(&dma_sem) == RT_EOK)
        ;
    dma_status = -1;
    conf.dst_addr = dbi_base + DBI_TXFIFO;
    conf.src_addr_width = conf.dst_addr_width = RT_DMA_SLAVE_BUSWIDTH_1_BYTE;
    /* the FIFO is a byte wide register: one byte per request */
    conf.src_maxburst = conf.dst_maxburst = 1;
    t.src_addr = (rt_ubase_t)buf;
    t.buffer_len = len;
    dbi_dma->callback = dma_cb;
    ret = rt_dma_chan_config(dbi_dma, &conf);
    if (!ret)
        ret = rt_dma_prep_single(dbi_dma, &t);
    if (ret)
        goto out_cs;
    wr(SPI_FIFO_CTL, rd(SPI_FIFO_CTL) | SPI_FIFO_TX_DRQEN);
    wr(DBI_CTRL2, rd(DBI_CTRL2) | DBI_CTRL2_DMA_ENABLE);
    ret = rt_dma_chan_start(dbi_dma);
    if (ret)
        goto out_dma;
    /* like the FIFO path, start the exchange with data already queued: the DMA fills the FIFO first */
    {
        rt_uint32_t i;

        for (i = 0; i < 1000u && ((rd(SPI_FIFO_STA) & SPI_FIFO_TX_COUNT_MASK) >> SPI_FIFO_TX_COUNT_SHIFT) < 16u; i++)
            rt_hw_us_delay(1u);
    }
    wr(SPI_TC, rd(SPI_TC) | SPI_TC_XCH);
    if (rt_sem_take(&dma_sem, rt_tick_from_millisecond(2000u)) != RT_EOK)
    {
        ret = -RT_ETIMEOUT;
        rt_dma_chan_stop(dbi_dma);
        goto out_dma;
    }
    if (dma_status < 0)
    {
        ret = dma_status;
        goto out_dma;
    }
    ret = wait_complete();

out_dma:
    wr(SPI_FIFO_CTL, rd(SPI_FIFO_CTL) & ~SPI_FIFO_TX_DRQEN);
    wr(DBI_CTRL2, rd(DBI_CTRL2) & ~DBI_CTRL2_DMA_ENABLE);
out_cs:
    wr(SPI_TC, rd(SPI_TC) | SPI_TC_SS_LEVEL);
    set_cs(RT_FALSE);
    return ret;
}

static int transfer(const rt_uint8_t *buf, rt_size_t len)
{
    if (len > SPI_FIFO_DEPTH)
        return transfer_dma(buf, len);
    while (len != 0u)
    {
        rt_size_t chunk = len < SPI_FIFO_DEPTH ? len : SPI_FIFO_DEPTH;
        int ret = transfer_chunk(buf, chunk);

        if (ret == -RT_ETIMEOUT)
            ret = transfer_chunk(buf, chunk);   /* a single byte burst can miss the first XCH edge */
        if (ret)
        {
            wr(SPI_TC, rd(SPI_TC) | SPI_TC_SS_LEVEL);
            return ret;
        }
        buf += chunk;
        len -= chunk;
    }
    return 0;
}

static void set_dcx(rt_bool_t data)
{
    rt_uint32_t reg = rd(DBI_CTRL1);

    if (data) reg |= DBI_CTRL1_DCX_DATA; else reg &= ~DBI_CTRL1_DCX_DATA;
    wr(DBI_CTRL1, reg);
}

int sun252i_dbi_command_write(rt_uint8_t cmd, const rt_uint8_t *data, rt_size_t len)
{
    int ret;

    if (!dbi_ready)
        return -RT_ERROR;
    rt_mutex_take(&dbi_lock, RT_WAITING_FOREVER);
    set_dcx(RT_FALSE);
    ret = transfer(&cmd, 1u);
    if (ret == 0 && len)
    {
        set_dcx(RT_TRUE);
        ret = transfer(data, len);
    }
    rt_mutex_release(&dbi_lock);
    return ret;
}

int sun252i_dbi_write_display(const rt_uint8_t *framebuf, rt_size_t size)
{
    int ret;

    if (!dbi_ready || !framebuf || !size)
        return -RT_EINVAL;
    rt_mutex_take(&dbi_lock, RT_WAITING_FOREVER);
    set_dcx(RT_TRUE);
    ret = transfer(framebuf, size);
    rt_mutex_release(&dbi_lock);
    return ret;
}

int sun252i_dbi_reset(rt_uint32_t delay_ms)
{
    if (pin_reset < 0)
        return -RT_ENOSYS;
    rt_pin_write(pin_reset, PIN_LOW);
    rt_thread_mdelay(delay_ms);
    rt_pin_write(pin_reset, PIN_HIGH);
    return 0;
}

static rt_err_t dbi_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct rt_clk *bus;
    struct rt_reset_control *rst;
    rt_uint32_t reg;

    dbi_base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    bus = rt_clk_get_by_name(dev, "bus");
    dbi_mod = rt_clk_get_by_name(dev, "mod");
    rst = rt_reset_control_get_by_index(dev, 0);
    dbi_dma = rt_dma_chan_request(dev, "tx");
    if (!dbi_base || rt_is_err_or_null(bus) || rt_is_err_or_null(dbi_mod) || rt_is_err_or_null(rst) ||
        rt_is_err_or_null(dbi_dma))
        return -RT_ERROR;
    rt_dm_dev_prop_read_u32(dev, "clock-frequency", &dbi_clock_hz);

    /* chip select and reset of the panel are plain GPIOs, absent when the pins belong to another interface */
    pin_cs = rt_pin_get_named_pin(dev, "cs", 0, RT_NULL, RT_NULL);
    pin_reset = rt_pin_get_named_pin(dev, "reset", 0, RT_NULL, RT_NULL);
    if (pin_cs >= 0)
    {
        rt_pin_mode(pin_cs, PIN_MODE_OUTPUT);
        rt_pin_write(pin_cs, PIN_HIGH);
    }
    if (pin_reset >= 0)
    {
        rt_pin_mode(pin_reset, PIN_MODE_OUTPUT);
        rt_pin_write(pin_reset, PIN_HIGH);
    }

    rt_mutex_init(&dbi_lock, "dbi", RT_IPC_FLAG_PRIO);
    rt_sem_init(&dma_sem, "dbi", 0, RT_IPC_FLAG_FIFO);

    rt_reset_control_assert(rst);
    rt_clk_prepare_enable(bus);
    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(dbi_mod);
    set_clock();

    reg = rd(SPI_GCR);
    reg |= SPI_GCR_ENABLE | SPI_GCR_MASTER | SPI_GCR_DBI_MODE | SPI_GCR_DBI_ENABLE;
    reg &= ~SPI_GCR_SOFT_RESET;
    wr(SPI_GCR, reg);
    wr(SPI_TC, SPI_TC_SPOL | SPI_TC_SDM | SPI_TC_DHB | SPI_TC_SS_OWNER);
    reg = rd(DBI_CTRL0);
    reg &= ~(DBI_CTRL0_INTERFACE | DBI_CTRL0_FORMAT);
    reg |= DBI_INTERFACE_D2LI << DBI_CTRL0_INTERFACE_SHIFT;
    wr(DBI_CTRL0, reg);
    wr(DBI_CTRL2, DBI_CTRL2_HRDY_BYPASS | DBI_CTRL2_DCX_PIN);
    wr(DBI_CTRL1, 0u);
    wr(DBI_SIZE, 0u);
    wr(DBI_INT, 0u);
    wr(SPI_INT_STA, 0xffffffffu);
    dbi_ready = RT_TRUE;

    return reset_fifo();
}

static const struct rt_ofw_node_id dbi_ofw_ids[] =
{
    { .compatible = "allwinner,sunxi-dbi" },
    { /* sentinel */ }
};

static struct rt_platform_driver dbi_driver =
{
    .name = "mipi-dbi-sun252i",
    .ids = dbi_ofw_ids,
    .probe = dbi_probe,
};
RT_PLATFORM_DRIVER_EXPORT(dbi_driver);

rt_bool_t sun252i_dbi_ready(void)
{
    return dbi_ready;
}
