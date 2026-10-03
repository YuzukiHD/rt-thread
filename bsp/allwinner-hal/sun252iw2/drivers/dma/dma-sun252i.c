/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMA controller of the sun252i SoC: twelve channels, each takes a chain of linked list
 * items (32 byte aligned); a cyclic transfer closes the last item back to the first one.
 * A device tree "dmas" specifier is <controller channel request-line>.
 */
#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>

#define DBG_TAG "dma.sun252i"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#include "dma-sun252i.h"

#define DMA_IRQ_EN          0x00u
#define DMA_IRQ_STAT        0x10u
#define DMA_SECURE          0x20u
#define DMA_COMMON_GATE     0x28u
#define DMA_CH_BASE         0x100u
#define DMA_CH_STRIDE       0x40u
#define DMA_ENABLE          0x00u
#define DMA_PAUSE           0x04u
#define DMA_LLI_ADDR        0x08u
#define DMA_CUR_SRC         0x10u
#define DMA_CUR_DST         0x14u
#define DMA_CNT             0x18u

#define DMA_IRQ_PACKAGE     RT_BIT(1)
#define DMA_IRQ_QUEUE       RT_BIT(2)
#define DMA_IRQ_TIMEOUT     RT_BIT(3)
#define DMA_IRQ_GROUPS      2u
#define DMA_CHANNELS        12u
#define DMA_LINK_END        0xfffff800u
#define DMA_MAX_BLOCK       0x00ffffffu
#define DMA_DRQ_SDRAM       0u
#define DMA_MAX_BLOCKS      32u
#define CACHE_LINE          64u

struct dma_lli
{
    rt_uint32_t cfg;
    rt_uint32_t src;
    rt_uint32_t dst;
    rt_uint32_t len;
    rt_uint32_t para;
    rt_uint32_t next;
} __attribute__((aligned(32)));

struct sun_dma;

struct sun_chan
{
    struct dma_lli lli[DMA_MAX_BLOCKS];
    struct rt_dma_chan chan;
    struct sun_dma *dma;
    rt_uint32_t idx;
    rt_uint32_t slot;
    rt_uint32_t blocks;
    rt_uint32_t block_done;
    rt_bool_t cyclic;
    rt_bool_t busy;
    enum rt_dma_transfer_direction dir;
} __attribute__((aligned(CACHE_LINE)));

struct sun_dma
{
    struct rt_dma_controller ctrl;
    void *base;
    int irq;
    struct sun_chan *chans[DMA_CHANNELS];
    struct rt_timer hi_timer;
    rt_bool_t hi_timer_on;
};

#define chan_to_sun(c) rt_container_of(c, struct sun_chan, chan)

static rt_uint32_t dma_rd(struct sun_dma *d, rt_uint32_t off)
{
    return HWREG32((rt_ubase_t)d->base + off);
}

static void dma_wr(struct sun_dma *d, rt_uint32_t off, rt_uint32_t v)
{
    HWREG32((rt_ubase_t)d->base + off) = v;
}

static rt_uint32_t ch_off(rt_uint32_t ch, rt_uint32_t off)
{
    return DMA_CH_BASE + ch * DMA_CH_STRIDE + off;
}

static rt_uint32_t width_code(enum rt_dma_slave_buswidth w)
{
    switch (w)
    {
    case RT_DMA_SLAVE_BUSWIDTH_1_BYTE: return 0;
    case RT_DMA_SLAVE_BUSWIDTH_2_BYTES: return 1;
    case RT_DMA_SLAVE_BUSWIDTH_4_BYTES: return 2;
    default: return 0xffffffffu;
    }
}

static rt_int32_t burst_code(rt_uint32_t beats)
{
    switch (beats)
    {
    case 0:
    case 1: return 0;
    case 4: return 1;
    case 8: return 2;
    case 16: return 3;
    default: return -1;
    }
}

static void dma_irq_enable_ch(struct sun_dma *d, rt_uint32_t ch, rt_bool_t enable, rt_bool_t per_block)
{
    rt_uint32_t group = ch / 8u;
    rt_uint32_t shift = (ch % 8u) * 4u;
    rt_uint32_t mask = (DMA_IRQ_QUEUE | DMA_IRQ_TIMEOUT | (per_block ? DMA_IRQ_PACKAGE : 0u)) << shift;
    rt_uint32_t reg = dma_rd(d, DMA_IRQ_EN + group * 4u);

    dma_wr(d, DMA_IRQ_EN + group * 4u, enable ? (reg | mask) : (reg & ~mask));
}

/* the data the device wrote is read through the cache again */
static void sync_dest(struct sun_chan *c, rt_uint32_t block)
{
    if (c->dir == RT_DMA_DEV_TO_MEM || c->dir == RT_DMA_MEM_TO_MEM)
    {
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)(rt_ubase_t)c->lli[block].dst, c->lli[block].len);
    }
}

static rt_size_t total_len(struct sun_chan *c)
{
    rt_size_t len = 0;

    for (rt_uint32_t i = 0; i < c->blocks; i++)
    {
        len += c->lli[i].len;
    }

    return len;
}

static void dma_isr(int vector, void *param)
{
    struct sun_dma *d = param;
    rt_uint32_t status[DMA_IRQ_GROUPS], ch;

    status[0] = dma_rd(d, DMA_IRQ_STAT);
    status[1] = dma_rd(d, DMA_IRQ_STAT + 4u);
    dma_wr(d, DMA_IRQ_STAT, status[0]);
    dma_wr(d, DMA_IRQ_STAT + 4u, status[1]);

    for (ch = 0; ch < DMA_CHANNELS; ch++)
    {
        rt_uint32_t event = (status[ch / 8u] >> ((ch % 8u) * 4u)) & 0xfu;
        struct sun_chan *c = d->chans[ch];
        rt_size_t size;

        if (!event || !c)
        {
            continue;
        }

        if (event & DMA_IRQ_TIMEOUT)
        {
            c->busy = RT_FALSE;
            dma_irq_enable_ch(d, ch, RT_FALSE, RT_TRUE);
            rt_dma_chan_done(&c->chan, 0);
        }
        else if (!(event & DMA_IRQ_QUEUE) && (event & DMA_IRQ_PACKAGE))
        {
            /* one period of a cyclic transfer */
            rt_uint32_t blk = c->block_done++ % c->blocks;

            sync_dest(c, blk);
            rt_dma_chan_done(&c->chan, c->lli[blk].len);
        }
        else if (event & DMA_IRQ_QUEUE)
        {
            size = total_len(c);
            c->busy = RT_FALSE;
            dma_irq_enable_ch(d, ch, RT_FALSE, RT_TRUE);
            for (rt_uint32_t i = 0; i < c->blocks; i++)
            {
                sync_dest(c, i);
            }
            rt_dma_chan_done(&c->chan, size);
        }
    }
}

/*
 * The status bits of channels 8..11 are set but the interrupt of the controller does not
 * fire for them: a timer polls the second status word while one of those channels runs.
 */
static void hi_poll(void *arg)
{
    struct sun_dma *d = arg;
    rt_bool_t busy = RT_FALSE;

    if (dma_rd(d, DMA_IRQ_STAT + 4u))
    {
        dma_isr(0, d);
    }
    for (rt_uint32_t ch = 8; ch < DMA_CHANNELS; ch++)
    {
        busy |= d->chans[ch] && d->chans[ch]->busy;
    }
    if (!busy)
    {
        rt_timer_stop(&d->hi_timer);
        d->hi_timer_on = RT_FALSE;
    }
}

/* ---- controller operations ------------------------------------------------------------ */
static struct rt_dma_chan *sun_dma_request_chan(struct rt_dma_controller *ctrl, struct rt_device *slave, void *fw_data)
{
    struct sun_dma *d = rt_container_of(ctrl, struct sun_dma, ctrl);
    struct rt_ofw_cell_args *args = fw_data;
    struct sun_chan *c;
    rt_uint32_t idx = DMA_CHANNELS, slot = DMA_DRQ_SDRAM;

    if (args)
    {
        if (args->args_count < 2 || args->args[0] >= DMA_CHANNELS)
        {
            return rt_err_ptr(-RT_EINVAL);
        }
        idx = args->args[0];
        slot = args->args[1];
        if (d->chans[idx])
        {
            return rt_err_ptr(-RT_EBUSY);
        }
    }
    else
    {
        /* memory to memory: any free channel with a working interrupt first */
        for (rt_uint32_t i = 0; i < DMA_CHANNELS; i++)
        {
            if (!d->chans[i])
            {
                idx = i;
                break;
            }
        }
        if (idx == DMA_CHANNELS)
        {
            return rt_err_ptr(-RT_EBUSY);
        }
    }

    if (!(c = rt_malloc_align(sizeof(*c), CACHE_LINE)))
    {
        return rt_err_ptr(-RT_ENOMEM);
    }
    rt_memset(c, 0, sizeof(*c));
    c->dma = d;
    c->idx = idx;
    c->slot = slot;
    d->chans[idx] = c;

    return &c->chan;
}

static rt_err_t sun_dma_release_chan(struct rt_dma_chan *chan)
{
    struct sun_chan *c = chan_to_sun(chan);

    c->dma->chans[c->idx] = RT_NULL;
    rt_free_align(c);

    return RT_EOK;
}

static rt_err_t sun_dma_config(struct rt_dma_chan *chan, struct rt_dma_slave_config *conf)
{
    RT_UNUSED(chan);
    RT_UNUSED(conf);

    return RT_EOK;
}

/* the common configuration word of the items of the channel */
static rt_err_t item_cfg(struct sun_chan *c, enum rt_dma_transfer_direction dir, rt_uint32_t *cfg)
{
    struct rt_dma_slave_config *conf = &c->chan.conf;
    rt_uint32_t sw = width_code(conf->src_addr_width), dw = width_code(conf->dst_addr_width);
    rt_int32_t sb = burst_code(conf->src_maxburst), db = burst_code(conf->dst_maxburst);

    if (dir == RT_DMA_MEM_TO_MEM && !conf->src_addr_width)
    {
        sw = dw = 2;
        sb = db = 0;
    }
    if (sw == 0xffffffffu || dw == 0xffffffffu || sb < 0 || db < 0 || sw != dw)
    {
        return -RT_EINVAL;
    }

    *cfg = (sw << 9) | (sb << 6) | (dw << 25) | (db << 22);
    switch (dir)
    {
    case RT_DMA_MEM_TO_MEM:
        *cfg |= (DMA_DRQ_SDRAM << 0) | (DMA_DRQ_SDRAM << 16);
        break;
    case RT_DMA_MEM_TO_DEV:
        *cfg |= (DMA_DRQ_SDRAM << 0) | (c->slot << 16) | RT_BIT(24);
        break;
    case RT_DMA_DEV_TO_MEM:
        *cfg |= (c->slot << 0) | (DMA_DRQ_SDRAM << 16) | RT_BIT(8);
        break;
    default:
        return -RT_EINVAL;
    }

    return RT_EOK;
}

static rt_err_t build_items(struct sun_chan *c, enum rt_dma_transfer_direction dir, rt_ubase_t buf,
                            rt_size_t buf_len, rt_size_t period_len, rt_bool_t cyclic)
{
    struct rt_dma_slave_config *conf = &c->chan.conf;
    rt_uint32_t cfg, blocks = period_len ? buf_len / period_len : 1;
    rt_err_t err;

    if (c->busy)
    {
        return -RT_EBUSY;
    }
    if (!blocks || blocks > DMA_MAX_BLOCKS || !(period_len ? period_len : buf_len) ||
        (period_len ? period_len : buf_len) > DMA_MAX_BLOCK)
    {
        return -RT_EINVAL;
    }
    if ((err = item_cfg(c, dir, &cfg)))
    {
        return err;
    }

    c->dir = dir;
    c->blocks = blocks;
    c->cyclic = cyclic;
    c->block_done = 0;

    for (rt_uint32_t i = 0; i < blocks; i++)
    {
        struct dma_lli *l = &c->lli[i];
        rt_size_t len = period_len ? period_len : buf_len;

        l->cfg = cfg;
        l->len = len;
        l->para = 64u;
        l->src = dir == RT_DMA_DEV_TO_MEM ? conf->src_addr : buf + i * len;
        l->dst = dir == RT_DMA_MEM_TO_DEV ? conf->dst_addr : buf + i * len;
        l->next = (i + 1u < blocks) ? (rt_uint32_t)&c->lli[i + 1u] : (cyclic ? (rt_uint32_t)&c->lli[0] : DMA_LINK_END);
    }

    return RT_EOK;
}

static rt_err_t sun_dma_prep_single(struct rt_dma_chan *chan, rt_ubase_t buf, rt_size_t len,
                                    enum rt_dma_transfer_direction dir)
{
    return build_items(chan_to_sun(chan), dir, buf, len, 0, RT_FALSE);
}

static rt_err_t sun_dma_prep_cyclic(struct rt_dma_chan *chan, rt_ubase_t buf, rt_size_t buf_len,
                                    rt_size_t period_len, enum rt_dma_transfer_direction dir)
{
    return build_items(chan_to_sun(chan), dir, buf, buf_len, period_len, RT_TRUE);
}

static rt_err_t sun_dma_prep_memcpy(struct rt_dma_chan *chan, rt_ubase_t src, rt_ubase_t dst, rt_size_t len)
{
    struct sun_chan *c = chan_to_sun(chan);
    rt_err_t err = build_items(c, RT_DMA_MEM_TO_MEM, src, len, 0, RT_FALSE);

    if (!err)
    {
        c->lli[0].src = src;
        c->lli[0].dst = dst;
    }

    return err;
}

static rt_err_t sun_dma_start(struct rt_dma_chan *chan)
{
    struct sun_chan *c = chan_to_sun(chan);
    struct sun_dma *d = c->dma;

    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, c->lli, sizeof(c->lli[0]) * c->blocks);
    if (c->dir != RT_DMA_DEV_TO_MEM)
    {
        for (rt_uint32_t i = 0; i < c->blocks; i++)
        {
            rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)(rt_ubase_t)c->lli[i].src, c->lli[i].len);
        }
    }
    c->busy = RT_TRUE;
    dma_irq_enable_ch(d, c->idx, RT_TRUE, c->cyclic);
    dma_wr(d, ch_off(c->idx, DMA_LLI_ADDR), (rt_uint32_t)&c->lli[0]);
    dma_wr(d, ch_off(c->idx, DMA_ENABLE), 1);
    if (c->idx >= 8 && !d->hi_timer_on)
    {
        d->hi_timer_on = RT_TRUE;
        rt_timer_start(&d->hi_timer);
    }

    return RT_EOK;
}

static rt_err_t sun_dma_stop(struct rt_dma_chan *chan)
{
    struct sun_chan *c = chan_to_sun(chan);
    struct sun_dma *d = c->dma;

    dma_wr(d, ch_off(c->idx, DMA_PAUSE), 1);
    dma_wr(d, ch_off(c->idx, DMA_ENABLE), 0);
    dma_wr(d, ch_off(c->idx, DMA_PAUSE), 0);
    dma_irq_enable_ch(d, c->idx, RT_FALSE, RT_TRUE);
    c->busy = RT_FALSE;

    return RT_EOK;
}

static const struct rt_dma_controller_ops sun_dma_ops =
{
    .request_chan = sun_dma_request_chan,
    .release_chan = sun_dma_release_chan,
    .start = sun_dma_start,
    .stop = sun_dma_stop,
    .config = sun_dma_config,
    .prep_memcpy = sun_dma_prep_memcpy,
    .prep_cyclic = sun_dma_prep_cyclic,
    .prep_single = sun_dma_prep_single,
};

rt_size_t sun252i_dma_pending(struct rt_dma_chan *chan)
{
    struct sun_chan *c = chan_to_sun(chan);

    return dma_rd(c->dma, ch_off(c->idx, DMA_CNT));
}

rt_size_t sun252i_dma_position(struct rt_dma_chan *chan)
{
    struct sun_chan *c = chan_to_sun(chan);
    rt_bool_t tx = c->dir != RT_DMA_DEV_TO_MEM;
    rt_uint32_t cur = dma_rd(c->dma, ch_off(c->idx, tx ? DMA_CUR_SRC : DMA_CUR_DST)), offset = 0;

    for (rt_uint32_t i = 0; i < c->blocks; i++)
    {
        rt_uint32_t start = tx ? c->lli[i].src : c->lli[i].dst;

        if (cur >= start && cur - start < c->lli[i].len)
        {
            return offset + (cur - start);
        }
        offset += c->lli[i].len;
    }

    return 0;
}

static rt_err_t sun_dma_probe(struct rt_platform_device *pdev)
{
    rt_err_t err;
    struct rt_device *dev = &pdev->parent;
    struct sun_dma *d = rt_calloc(1, sizeof(*d));
    struct rt_clk *bus, *mbus;
    struct rt_reset_control *rst;

    if (!d)
    {
        return -RT_ENOMEM;
    }

    d->base = rt_dm_dev_iomap(dev, 0);
    if (!d->base)
    {
        err = -RT_EIO;
        goto _fail;
    }
    if ((d->irq = rt_dm_dev_get_irq(dev, 0)) < 0)
    {
        err = d->irq;
        goto _fail;
    }

    bus = rt_clk_get_by_name(dev, "bus");
    mbus = rt_clk_get_by_name(dev, "mbus");
    rst = rt_reset_control_get_by_index(dev, 0);
    if (rt_is_err_or_null(bus) || rt_is_err_or_null(mbus) || rt_is_err_or_null(rst))
    {
        err = -RT_ERROR;
        goto _fail;
    }
    rt_reset_control_assert(rst);
    rt_clk_prepare_enable(bus);
    rt_clk_prepare_enable(mbus);
    rt_reset_control_deassert(rst);

    /* automatic gating off, every channel non secure, interrupt status clear */
    dma_wr(d, DMA_COMMON_GATE, 0x7u);
    dma_wr(d, DMA_SECURE, RT_BIT(DMA_CHANNELS) - 1u);
    for (rt_uint32_t i = 0; i < DMA_IRQ_GROUPS; i++)
    {
        dma_wr(d, DMA_IRQ_EN + i * 4u, 0);
        dma_wr(d, DMA_IRQ_STAT + i * 4u, 0xffffffffu);
    }

    rt_timer_init(&d->hi_timer, "dmahi", hi_poll, d, 1, RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    rt_hw_interrupt_install(d->irq, dma_isr, d, "dma");
    rt_hw_interrupt_umask(d->irq);

    d->ctrl.dev = dev;
    d->ctrl.ops = &sun_dma_ops;
    d->ctrl.addr_mask = RT_DMA_ADDR_MASK(32);
    rt_bitmap_set_bit(d->ctrl.dir_cap, RT_DMA_MEM_TO_MEM);
    rt_bitmap_set_bit(d->ctrl.dir_cap, RT_DMA_MEM_TO_DEV);
    rt_bitmap_set_bit(d->ctrl.dir_cap, RT_DMA_DEV_TO_MEM);

    if ((err = rt_dma_controller_register(&d->ctrl)))
    {
        goto _fail;
    }

    return RT_EOK;

_fail:
    rt_free(d);

    return err;
}

static const struct rt_ofw_node_id sun_dma_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-dma" },
    { /* sentinel */ }
};

static struct rt_platform_driver sun_dma_driver =
{
    .name = "dma-sun252i",
    .ids = sun_dma_ofw_ids,
    .probe = sun_dma_probe,
};

static int sun_dma_drv_register(void)
{
    rt_platform_driver_register(&sun_dma_driver);

    return 0;
}
INIT_SUBSYS_EXPORT(sun_dma_drv_register);
