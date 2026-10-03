/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD/MMC host controller (SMHC0, the TF card slot).
 *
 * Commands and data go through a 32 bit FIFO that is drained either by the
 * CPU or by the internal DMA engine (IDMAC), which walks a chain of 16 byte
 * descriptors of up to 8 KiB each. A request is started with one write to the
 * command register and completes in the interrupt handler. The module clock
 * runs at twice the card clock (four times for DDR); sampling and drive
 * phases are picked by frequency range.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <string.h>
#include <drivers/dev_mmcsd_core.h>
#include <drivers/dev_pin.h>


#define SMHC_GCTRL      0x00u
#define SMHC_CLKCR      0x04u
#define SMHC_TMOUT      0x08u
#define SMHC_WIDTH      0x0cu
#define SMHC_BLKSZ      0x10u
#define SMHC_BCNTR      0x14u
#define SMHC_CMDR       0x18u
#define SMHC_CARG       0x1cu
#define SMHC_RESP0      0x20u
#define SMHC_IMASK      0x30u
#define SMHC_MISTA      0x34u
#define SMHC_RINTR      0x38u
#define SMHC_STAS       0x3cu
#define SMHC_FTRGL      0x40u
#define SMHC_A12A       0x58u
#define SMHC_NTSR       0x5cu
#define SMHC_DMAC       0x80u
#define SMHC_DLBA       0x84u
#define SMHC_IDST       0x88u
#define SMHC_IDIE       0x8cu
#define SMHC_DRV_DL     0x140u
#define SMHC_FIFO       0x200u

#define BIT(n)          (1u << (n))
#define GCTRL_SOFT_RST  BIT(0)
#define GCTRL_FIFO_RST  BIT(1)
#define GCTRL_DMA_RST   BIT(2)
#define GCTRL_ALL_RST   (GCTRL_SOFT_RST | GCTRL_FIFO_RST | GCTRL_DMA_RST)
#define GCTRL_INT_EN    BIT(4)
#define GCTRL_DMA_EN    BIT(5)
#define GCTRL_DDR_MODE  BIT(10)
#define GCTRL_ACCESS_DONE_DIR BIT(30)
#define GCTRL_ACCESS_BY_AHB   BIT(31)

#define CLKCR_DIV_MASK  0xffu
#define CLKCR_CARD_CLK_ON BIT(16)
#define CLKCR_LOW_POWER BIT(17)
#define CLKCR_MASK_DATA0 BIT(31)

#define CMDR_RSP_EXP    BIT(6)
#define CMDR_LONG_RSP   BIT(7)
#define CMDR_CHECK_CRC  BIT(8)
#define CMDR_DATA_EXP   BIT(9)
#define CMDR_WRITE      BIT(10)
#define CMDR_WAIT_PRE_OVER BIT(13)
#define CMDR_SEND_INIT_SEQ BIT(15)
#define CMDR_UPCLK_ONLY BIT(21)
#define CMDR_START      BIT(31)

#define INT_RESP_ERR    BIT(1)
#define INT_CMD_DONE    BIT(2)
#define INT_DATA_OVER   BIT(3)
#define INT_RESP_CRC_ERR BIT(6)
#define INT_DATA_CRC_ERR BIT(7)
#define INT_RESP_TIMEOUT BIT(8)
#define INT_DATA_TIMEOUT BIT(9)
#define INT_FIFO_RUN_ERR BIT(11)
#define INT_HARD_LOCKED BIT(12)
#define INT_START_BIT_ERR BIT(13)
#define INT_END_BIT_ERR BIT(15)
#define INT_ERR_MASK    (INT_RESP_ERR | INT_RESP_CRC_ERR | INT_DATA_CRC_ERR | INT_RESP_TIMEOUT | \
                         INT_DATA_TIMEOUT | INT_FIFO_RUN_ERR | INT_HARD_LOCKED | \
                         INT_START_BIT_ERR | INT_END_BIT_ERR)

#define STAS_FIFO_EMPTY BIT(2)
#define STAS_FIFO_FULL  BIT(3)
#define STAS_CARD_BUSY  BIT(9)

#define DMAC_SOFT_RST   BIT(0)
#define DMAC_FIX_BURST  BIT(1)
#define DMAC_ON         BIT(7)

#define IDST_RX_INT     BIT(1)
#define IDST_ERR_MASK   (BIT(2) | BIT(4) | BIT(5) | BIT(9))
#define IDST_ALL        0x337u

#define NTSR_CMD_PH_MASK (3u << 4)
#define NTSR_DAT_PH_MASK (3u << 8)
#define NTSR_2X_TIMING  BIT(31)
#define DRV_DL_CMD_PH   BIT(16)
#define DRV_DL_DAT_PH   BIT(17)

#define DES_CFG_DIC     BIT(1)
#define DES_CFG_LD      BIT(2)
#define DES_CFG_FD      BIT(3)
#define DES_CFG_CH      BIT(4)
#define DES_CFG_ER      BIT(5)
#define DES_CFG_OWN     BIT(31)
#define DES_MAX_LEN     8192u
#define DES_COUNT       8u

#define HOSC_RATE       24000000u
#define DMA_ALIGN       64u
#define BOUNCE_SIZE     (DES_COUNT * DES_MAX_LEN)
#define BOUNCE_MIN      512u
#define RX_WATERMARK    7u
#define TX_WATERMARK    248u
#define BURST_SIZE      2u

struct smhc_desc
{
    rt_uint32_t config;
    rt_uint32_t size;
    rt_uint32_t buf_addr;
    rt_uint32_t next_addr;
};

static struct smhc
{
    struct rt_mmcsd_host *host;
    rt_ubase_t base;
    struct rt_clk *mod;
    rt_base_t cd_pin;
    struct rt_semaphore done;
    struct { rt_uint32_t clock; rt_uint8_t width; rt_uint8_t timing; } ios;
    volatile rt_uint32_t need, rint, idst, need_idst;
    volatile rt_bool_t active;
    rt_uint8_t *bounce;
    struct smhc_desc desc[DES_COUNT] __attribute__((aligned(DMA_ALIGN)));
} smhc;

static inline rt_uint32_t rd(rt_uint32_t off) { return HWREG32(smhc.base + off); }
static inline void wr(rt_uint32_t off, rt_uint32_t v) { HWREG32(smhc.base + off) = v; }

static rt_err_t wait_clear(rt_uint32_t off, rt_uint32_t mask)
{
    rt_uint32_t i;

    for (i = 0; i < 100000u; i++)
    {
        if ((rd(off) & mask) == 0u)
            return RT_EOK;
        rt_hw_us_delay(1u);
    }
    return -RT_ETIMEOUT;
}

static rt_err_t update_clock(void)
{
    rt_uint32_t clkcr = rd(SMHC_CLKCR);
    rt_err_t ret;

    wr(SMHC_CLKCR, clkcr | CLKCR_MASK_DATA0);
    wr(SMHC_CMDR, CMDR_START | CMDR_UPCLK_ONLY | CMDR_WAIT_PRE_OVER);
    ret = wait_clear(SMHC_CMDR, CMDR_START);
    wr(SMHC_RINTR, 0xffffffffu);
    wr(SMHC_CLKCR, clkcr & ~CLKCR_MASK_DATA0);
    return ret;
}

static void set_phase(rt_uint32_t hz)
{
    rt_uint32_t drv = rd(SMHC_DRV_DL);
    rt_uint32_t ntsr = rd(SMHC_NTSR);
    rt_uint32_t sample;

    drv |= DRV_DL_CMD_PH;
    if (hz > 26000000u && hz <= 52000000u)
    {
        drv |= DRV_DL_DAT_PH;
        sample = 1u;
    }
    else
    {
        drv &= ~DRV_DL_DAT_PH;
        sample = 0u;
    }
    wr(SMHC_DRV_DL, drv);
    ntsr &= ~(NTSR_CMD_PH_MASK | NTSR_DAT_PH_MASK);
    ntsr |= (sample << 4) | (sample << 8) | NTSR_2X_TIMING;
    wr(SMHC_NTSR, ntsr);
}

static rt_err_t set_clock(rt_uint32_t hz, rt_bool_t ddr)
{
    rt_uint32_t clkcr = rd(SMHC_CLKCR) & ~(CLKCR_CARD_CLK_ON | CLKCR_LOW_POWER);
    rt_uint32_t target, gctrl;
    rt_err_t ret;

    wr(SMHC_CLKCR, clkcr);
    ret = update_clock();
    if (ret != RT_EOK || hz == 0u)
        return ret;

    target = hz * (ddr ? 4u : 2u);
    gctrl = rd(SMHC_GCTRL);
    if (ddr) gctrl |= GCTRL_DDR_MODE; else gctrl &= ~GCTRL_DDR_MODE;
    wr(SMHC_GCTRL, gctrl);
    ret = rt_clk_set_rate(smhc.mod, target);
    if (ret != RT_EOK)
        return ret;

    clkcr &= ~CLKCR_DIV_MASK;
    if (ddr) clkcr |= 1u;
    wr(SMHC_CLKCR, clkcr);
    set_phase(hz);
    wr(SMHC_CLKCR, clkcr | CLKCR_CARD_CLK_ON);
    return update_clock();
}

static void smhc_isr(int vector, void *param)
{
    rt_uint32_t msk = rd(SMHC_MISTA);
    rt_uint32_t idst = rd(SMHC_IDST);

    (void)vector; (void)param;
    wr(SMHC_RINTR, msk);
    wr(SMHC_IDST, idst);
    if (!smhc.active)
        return;

    smhc.rint |= msk;
    smhc.idst |= idst;
    if ((smhc.rint & INT_ERR_MASK) || (smhc.idst & IDST_ERR_MASK) ||
        ((smhc.rint & smhc.need) == smhc.need && (smhc.idst & smhc.need_idst) == smhc.need_idst))
    {
        wr(SMHC_IMASK, 0u);
        rt_sem_release(&smhc.done);
    }
}

static rt_err_t reset_ctrl(void)
{
    rt_err_t ret;

    wr(SMHC_GCTRL, rd(SMHC_GCTRL) | GCTRL_ALL_RST | GCTRL_INT_EN | GCTRL_ACCESS_DONE_DIR);
    ret = wait_clear(SMHC_GCTRL, GCTRL_ALL_RST);
    wr(SMHC_RINTR, 0xffffffffu);
    wr(SMHC_IDST, IDST_ALL);
    wr(SMHC_IMASK, 0u);
    wr(SMHC_TMOUT, (0xffffffu << 8) | 0xffu);
    return ret;
}

static void recover(void)
{
    reset_ctrl();
    wr(SMHC_WIDTH, smhc.ios.width);
    set_clock(smhc.ios.clock, smhc.ios.timing == MMCSD_TIMING_UHS_DDR50 ||
                              smhc.ios.timing == MMCSD_TIMING_MMC_DDR52);
}

static rt_uint32_t cmd_flags(struct rt_mmcsd_cmd *cmd)
{
    rt_uint32_t flags = CMDR_START | (cmd->cmd_code & 0x3fu);

    switch (resp_type(cmd))
    {
    case RESP_NONE:
        break;
    case RESP_R2:
        flags |= CMDR_RSP_EXP | CMDR_LONG_RSP | CMDR_CHECK_CRC;
        break;
    case RESP_R3:
    case RESP_R4:
        flags |= CMDR_RSP_EXP;
        break;
    default:
        flags |= CMDR_RSP_EXP | CMDR_CHECK_CRC;
        break;
    }
    if (cmd->cmd_code == GO_IDLE_STATE)
        flags |= CMDR_SEND_INIT_SEQ;
    return flags;
}

static rt_err_t build_desc(const void *buf, rt_uint32_t len)
{
    rt_uint32_t n = (len + DES_MAX_LEN - 1u) / DES_MAX_LEN, i;
    rt_uint32_t addr = (rt_uint32_t)buf;

    if (n > DES_COUNT)
        return -RT_EINVAL;
    for (i = 0; i < n; i++)
    {
        struct smhc_desc *d = &smhc.desc[i];
        rt_uint32_t chunk = len - i * DES_MAX_LEN;
        rt_uint32_t cfg = DES_CFG_CH | DES_CFG_OWN | DES_CFG_DIC;

        if (chunk > DES_MAX_LEN) chunk = DES_MAX_LEN;
        if (i == 0) cfg |= DES_CFG_FD;
        d->size = chunk;
        d->buf_addr = (addr + i * DES_MAX_LEN) >> 2;
        if (i == n - 1u)
        {
            cfg = (cfg & ~DES_CFG_DIC) | DES_CFG_LD | DES_CFG_ER;
            d->next_addr = 0;
        }
        else
        {
            d->next_addr = (rt_uint32_t)&smhc.desc[i + 1u] >> 2;
        }
        d->config = cfg;
    }
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, smhc.desc, n * sizeof(struct smhc_desc));
    return RT_EOK;
}

static void dma_start(rt_bool_t read)
{
    rt_uint32_t gctrl = rd(SMHC_GCTRL) & ~GCTRL_ACCESS_BY_AHB;

    wr(SMHC_GCTRL, gctrl | GCTRL_DMA_EN);
    wr(SMHC_GCTRL, gctrl | GCTRL_DMA_EN | GCTRL_DMA_RST | GCTRL_FIFO_RST);
    wait_clear(SMHC_GCTRL, GCTRL_DMA_RST | GCTRL_FIFO_RST);
    wr(SMHC_DMAC, DMAC_SOFT_RST);
    wait_clear(SMHC_DMAC, DMAC_SOFT_RST);
    wr(SMHC_DMAC, DMAC_FIX_BURST | DMAC_ON);
    wr(SMHC_IDIE, read ? IDST_RX_INT : 0u);
    wr(SMHC_DLBA, (rt_uint32_t)smhc.desc >> 2);
    wr(SMHC_FTRGL, (BURST_SIZE << 28) | (RX_WATERMARK << 16) | TX_WATERMARK);
}

static void dma_stop(void)
{
    wr(SMHC_IDST, IDST_ALL);
    wr(SMHC_IDIE, 0);
    wr(SMHC_DMAC, 0);
    wr(SMHC_GCTRL, (rd(SMHC_GCTRL) | GCTRL_DMA_RST) & ~GCTRL_DMA_EN);
}

static rt_err_t fifo_wait(rt_uint32_t flag)
{
    rt_tick_t start = rt_tick_get();

    while (rd(SMHC_STAS) & flag)
    {
        if (rt_tick_get() - start > RT_TICK_PER_SECOND)
            return -RT_ETIMEOUT;
        if (rd(SMHC_RINTR) & INT_ERR_MASK)
            return -RT_EIO;
    }
    return RT_EOK;
}

static rt_err_t pio(rt_uint8_t *buf, rt_uint32_t len, rt_bool_t write)
{
    rt_uint32_t i = 0;

    while (i < len)
    {
        rt_uint32_t w = 0;
        rt_uint32_t n = len - i < 4u ? len - i : 4u;
        rt_err_t ret = fifo_wait(write ? STAS_FIFO_FULL : STAS_FIFO_EMPTY);

        if (ret != RT_EOK)
            return ret;
        if (write)
        {
            memcpy(&w, buf + i, n);
            wr(SMHC_FIFO, w);
        }
        else
        {
            w = rd(SMHC_FIFO);
            memcpy(buf + i, &w, n);
        }
        i += n;
    }
    return RT_EOK;
}

static rt_err_t wait_idle(rt_uint32_t ms)
{
    rt_uint32_t i;

    /* the card normally releases the data line within microseconds: poll finely first,
     * sleeping only when it stays busy (the system tick is 10 ms) */
    for (i = 0; i < 20000u; i++)
    {
        if ((rd(SMHC_STAS) & STAS_CARD_BUSY) == 0u)
            return RT_EOK;
        rt_hw_us_delay(10u);
    }
    for (i = 0; i < ms; i++)
    {
        if ((rd(SMHC_STAS) & STAS_CARD_BUSY) == 0u)
            return RT_EOK;
        rt_thread_mdelay(1);
    }
    return -RT_EBUSY;
}

static rt_err_t xfer(struct rt_mmcsd_cmd *cmd, struct rt_mmcsd_data *data)
{
    rt_uint32_t cmdr = cmd_flags(cmd);
    rt_uint32_t imask = INT_ERR_MASK;
    rt_bool_t use_dma = RT_FALSE, write = RT_FALSE;
    void *dma_buf = RT_NULL;
    rt_uint32_t len = 0;
    rt_int32_t timeout_ms = 1000;
    rt_err_t ret;

    ret = wait_idle(2000);
    if (ret != RT_EOK)
        return ret;
    if (rd(SMHC_GCTRL) & GCTRL_ALL_RST)
        return -RT_EBUSY;

    smhc.need = smhc.need_idst = smhc.rint = smhc.idst = 0u;
    while (rt_sem_trytake(&smhc.done) == RT_EOK)
        ;
    wr(SMHC_RINTR, 0xffffffffu);
    wr(SMHC_IDST, IDST_ALL);

    if (data != RT_NULL)
    {
        len = data->blksize * data->blks;
        write = (data->flags & DATA_DIR_WRITE) != 0u;
        timeout_ms += 1000;
        wr(SMHC_BLKSZ, data->blksize);
        wr(SMHC_BCNTR, len);
        cmdr |= CMDR_DATA_EXP | CMDR_WAIT_PRE_OVER;
        if (write)
            cmdr |= CMDR_WRITE;
        smhc.need = INT_DATA_OVER;

        if (len > 4u && len <= BOUNCE_SIZE)
        {
            if (((rt_uint32_t)data->buf % DMA_ALIGN) == 0u && (len % DMA_ALIGN) == 0u)
            {
                dma_buf = data->buf;
            }
            else if (len >= BOUNCE_MIN)
            {
                if (smhc.bounce == RT_NULL)
                    smhc.bounce = rt_malloc_align(BOUNCE_SIZE, DMA_ALIGN);
                dma_buf = smhc.bounce;
                if (dma_buf != RT_NULL && write)
                    memcpy(dma_buf, data->buf, len);
            }
        }
        use_dma = dma_buf != RT_NULL;
        if (use_dma)
        {
            rt_uint32_t flush = (len + DMA_ALIGN - 1u) & ~(DMA_ALIGN - 1u);

            rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, dma_buf, flush);
            if (build_desc(dma_buf, len) != RT_EOK)
                return -RT_EINVAL;
            dma_start(!write);
            if (!write)
                smhc.need_idst = IDST_RX_INT;
        }
        else
        {
            wr(SMHC_GCTRL, rd(SMHC_GCTRL) | GCTRL_ACCESS_BY_AHB | GCTRL_FIFO_RST);
            wait_clear(SMHC_GCTRL, GCTRL_FIFO_RST);
        }
        imask |= smhc.need;
    }
    else
    {
        smhc.need = INT_CMD_DONE;
        imask |= INT_CMD_DONE;
    }

    wr(SMHC_A12A, 0xffffu);
    wr(SMHC_CARG, cmd->arg);
    smhc.active = RT_TRUE;
    wr(SMHC_IMASK, imask);
    wr(SMHC_CMDR, cmdr);

    ret = RT_EOK;
    if (data != RT_NULL && !use_dma)
    {
        ret = pio((rt_uint8_t *)data->buf, len, write);
        if (ret != RT_EOK)
            goto out;
    }
    if (rt_sem_take(&smhc.done, rt_tick_from_millisecond(timeout_ms)) != RT_EOK)
        ret = -RT_ETIMEOUT;

out:
    smhc.active = RT_FALSE;
    wr(SMHC_IMASK, 0u);

    if (ret == RT_EOK)
    {
        if (smhc.rint & (INT_RESP_TIMEOUT | INT_DATA_TIMEOUT))
            ret = -RT_ETIMEOUT;
        else if ((smhc.rint & INT_ERR_MASK) || (smhc.idst & IDST_ERR_MASK))
            ret = -RT_EIO;
        else if ((smhc.rint & smhc.need) != smhc.need)
            ret = -RT_EIO;
    }

    if (data != RT_NULL)
    {
        if (use_dma)
        {
            dma_stop();
            if (!write)
            {
                rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, dma_buf, (len + DMA_ALIGN - 1u) & ~(DMA_ALIGN - 1u));
                if (dma_buf != data->buf && ret == RT_EOK)
                    memcpy(data->buf, dma_buf, len);
            }
        }
        wr(SMHC_GCTRL, rd(SMHC_GCTRL) | GCTRL_FIFO_RST);
    }

    if (ret == RT_EOK)
    {
        if (resp_type(cmd) == RESP_R2)
        {
            int i;
            for (i = 0; i < 4; i++)
                cmd->resp[3 - i] = rd(SMHC_RESP0 + 4u * i);
        }
        else
        {
            cmd->resp[0] = rd(SMHC_RESP0);
        }
        if (data != RT_NULL)
            data->bytes_xfered = len;
        if (resp_type(cmd) == RESP_R1B)
            wait_idle(2000);
    }
    else
    {
        /* a failed command, a response timeout included, leaves the command path out of step:
         * the following responses would arrive one command late */
        recover();
    }
    return ret;
}

static void smhc_request(struct rt_mmcsd_host *host, struct rt_mmcsd_req *req)
{
    rt_err_t ret;

    ret = xfer(req->cmd, req->data);
    req->cmd->err = ret;
    if (req->data)
        req->data->err = ret;
    if (req->stop && ret == RT_EOK)
    {
        rt_err_t sret = xfer(req->stop, RT_NULL);

        req->stop->err = sret;
    }
    else if (req->stop && ret != RT_EOK)
    {
        /* leave the card in a defined state */
        xfer(req->stop, RT_NULL);
    }
    mmcsd_req_complete(host);
}

static void smhc_set_iocfg(struct rt_mmcsd_host *host, struct rt_mmcsd_io_cfg *io)
{
    rt_bool_t ddr = io->timing == MMCSD_TIMING_UHS_DDR50 || io->timing == MMCSD_TIMING_MMC_DDR52;
    rt_uint32_t clock = io->clock;

    if (io->power_mode == MMCSD_POWER_OFF)
        clock = 0u;
    switch (io->bus_width)
    {
    case MMCSD_BUS_WIDTH_4: wr(SMHC_WIDTH, 1u); smhc.ios.width = 1u; break;
    case MMCSD_BUS_WIDTH_8: wr(SMHC_WIDTH, 2u); smhc.ios.width = 2u; break;
    default: wr(SMHC_WIDTH, 0u); smhc.ios.width = 0u; break;
    }
    if (clock != smhc.ios.clock || io->timing != smhc.ios.timing)
    {
        if (set_clock(clock, ddr) != RT_EOK)
            rt_kprintf("sdmmc: clock %u failed\n", clock);
        smhc.ios.clock = clock;
        smhc.ios.timing = io->timing;
    }
}

static rt_int32_t smhc_get_card_status(struct rt_mmcsd_host *host)
{
    return rt_pin_read(smhc.cd_pin) == PIN_LOW ? 1 : 0;
}

static void smhc_enable_sdio_irq(struct rt_mmcsd_host *host, rt_int32_t en)
{
    (void)host; (void)en;
}

static rt_bool_t smhc_card_busy(struct rt_mmcsd_host *host)
{
    return (rd(SMHC_STAS) & STAS_CARD_BUSY) ? RT_TRUE : RT_FALSE;
}

static const struct rt_mmcsd_host_ops smhc_ops =
{
    smhc_request,
    smhc_set_iocfg,
    smhc_get_card_status,
    smhc_enable_sdio_irq,
    RT_NULL,
    smhc_card_busy,
    RT_NULL,
};

static void cd_isr(void *arg)
{
    mmcsd_change(smhc.host);
}

static rt_err_t smhc_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct rt_mmcsd_host *host;
    struct rt_clk *bus;
    struct rt_reset_control *rst;
    rt_uint8_t cd_mode, cd_value;
    int irq;

    smhc.base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    irq = rt_dm_dev_get_irq(dev, 0);
    bus = rt_clk_get_by_name(dev, "ahb");
    smhc.mod = rt_clk_get_by_name(dev, "mod");
    rst = rt_reset_control_get_by_index(dev, 0);
    if (!smhc.base || irq < 0 || rt_is_err_or_null(bus) || rt_is_err_or_null(smhc.mod) || rt_is_err_or_null(rst))
    {
        return -RT_ERROR;
    }

    /* clock, reset: the pins come from the pin group of the node */
    rt_reset_control_assert(rst);
    rt_clk_prepare_enable(bus);
    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(smhc.mod);

    smhc.cd_pin = rt_ofw_get_named_pin(dev->ofw_node, "cd", 0, &cd_mode, &cd_value);
    if (smhc.cd_pin >= 0)
    {
        rt_pin_mode(smhc.cd_pin, PIN_MODE_INPUT_PULLUP);
    }

    rt_sem_init(&smhc.done, "smhc", 0, RT_IPC_FLAG_FIFO);
    if (reset_ctrl() != RT_EOK)
    {
        rt_kprintf("sdmmc: controller reset failed\n");
        return -RT_ERROR;
    }
    wr(SMHC_WIDTH, 0u);
    rt_hw_interrupt_install(irq, smhc_isr, RT_NULL, "smhc0");
    rt_hw_interrupt_umask(irq);

    host = mmcsd_alloc_host();
    if (!host)
        return -RT_ENOMEM;
    rt_strncpy(host->name, "sd", RT_NAME_MAX);
    host->ops = &smhc_ops;
    host->freq_min = 400000u;
    host->freq_max = 50000000u;
    host->valid_ocr = VDD_32_33 | VDD_33_34;
    host->flags = MMCSD_BUSWIDTH_4 | MMCSD_MUTBLKWRITE | MMCSD_SUP_HIGHSPEED;
    host->max_seg_size = DES_MAX_LEN;
    host->max_dma_segs = DES_COUNT;
    host->max_blk_size = 512u;
    host->max_blk_count = BOUNCE_SIZE / 512u;
    host->private_data = &smhc;
    smhc.host = host;

    if (smhc.cd_pin >= 0)
    {
        rt_pin_attach_irq(smhc.cd_pin, PIN_IRQ_MODE_RISING_FALLING, cd_isr, RT_NULL);
        rt_pin_irq_enable(smhc.cd_pin, PIN_IRQ_ENABLE);
    }
    if (smhc_get_card_status(host))
        mmcsd_change(host);

    return RT_EOK;
}

static const struct rt_ofw_node_id smhc_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-mmc" },
    { /* sentinel */ }
};

static struct rt_platform_driver smhc_driver =
{
    .name = "sdio-sun252i",
    .ids = smhc_ofw_ids,
    .probe = smhc_probe,
};
RT_PLATFORM_DRIVER_EXPORT(smhc_driver);
