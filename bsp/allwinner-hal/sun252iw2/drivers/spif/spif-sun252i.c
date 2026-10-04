/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI NOR flash controller. A command is four words (phases, address, bus widths and
 * opcodes, counts); without data it runs from the registers, with data from a chain of
 * 32 byte DMA descriptors (data bounce buffer 4 KiB, polled, no interrupt). Reads are
 * 1-1-4 (0x6b, the quad enable bit is set at init per manufacturer), programs 1-1-4
 * (0x32), 4 byte opcodes above 16 MiB.
 *
 * Tuning: a bad sample point makes the one wire status read return junk (the work in
 * progress bit reads 0: erase and program then look like they were ignored and later reads
 * return garbage), so the point is never left in a bad state, and the parameter records
 * are read at 24 MHz.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <string.h>
#include <dt-bindings/clock/sun252iw2-ccu.h>
#include "sunxi-util.h"
#include "spif-sun252i.h"

#define SPIF_VER        0x00u
#define SPIF_GC         0x04u
#define SPIF_GCA        0x08u
#define SPIF_TC         0x0cu
#define SPIF_INT_EN     0x14u
#define SPIF_INT_STA    0x18u
#define SPIF_CSD        0x1cu
#define SPIF_PHC        0x20u   /* phases of the command */
#define SPIF_TCF        0x24u   /* flash address */
#define SPIF_TCS        0x28u   /* bus widths and opcodes */
#define SPIF_TNM        0x2cu   /* counts */
#define SPIF_PSA        0x34u   /* prefetch window start */
#define SPIF_PEA        0x38u   /* prefetch window end */
#define SPIF_PMA        0x3cu   /* flash address of the window start */
#define SPIF_DMA_CTL    0x40u
#define SPIF_DSC        0x44u

#define GC_DMA_MODE     RT_BIT(0)
#define GC_ADDR_MAP     RT_BIT(1)
#define GC_NMODE_EN     RT_BIT(2)
#define GC_PMODE_EN     RT_BIT(3)
#define GC_CPHA         RT_BIT(4)
#define GC_CPOL         RT_BIT(5)
#define GC_SS_MASK      (RT_BIT(6) | RT_BIT(7))
#define GC_CS_POL       RT_BIT(8)
#define GC_HOLD_EN      RT_BIT(13)
#define GC_WP_EN        RT_BIT(15)
#define GC_DTR_EN       RT_BIT(16)
#define GC_RX_FBS       RT_BIT(17)
#define GC_TX_FBS       RT_BIT(18)

#define GCA_FIFO_RST    (RT_BIT(0) | RT_BIT(1))
#define GCA_SOFT_RST    RT_BIT(3)
#define GCA_DMA_END     RT_BIT(4)

#define TC_DELAY_MASK   0x3fu
#define TC_DELAY_SW_EN  RT_BIT(6)
#define TC_MODE_SHIFT   16
#define TC_MODE_MASK    (0x7u << TC_MODE_SHIFT)
#define TC_SAMPLE_EN    RT_BIT(20)
#define TC_SCKOUT_SEL   RT_BIT(26)

#define INT_ERR         (RT_BIT(8) | RT_BIT(9) | RT_BIT(10))
#define INT_DMA_DONE    RT_BIT(24)

#define CSD_DEFAULT     ((5u << 16) | (6u << 8) | 6u)

#define PHC_RX          RT_BIT(8)
#define PHC_TX          RT_BIT(12)
#define PHC_DUMMY       RT_BIT(16)
#define PHC_MODE        RT_BIT(20)
#define PHC_ADDR        RT_BIT(24)
#define PHC_CMD         RT_BIT(28)

#define TCS_DATA_SHIFT  0
#define TCS_MODE_SHIFT  2
#define TCS_MODE_OPCODE_SHIFT 16
#define TCS_ADDR_SHIFT  4
#define TCS_OPCODE_SHIFT 24

#define TNM_DUMMY_SHIFT 16
#define TNM_NORMAL_EN   RT_BIT(28)
#define TNM_LEN_64K     RT_BIT(31)

#define DMA_CTL_START   RT_BIT(0)
#define DMA_CTL_DESC_LEN_MASK (0xffu << 4)
#define DMA_DESC_BYTES  32u

#define DESC_LAST       RT_BIT(0)
#define DESC_READ       RT_BIT(1)       /* the DMA writes memory */
#define DESC_BURST_INCR16 (7u << 4)
#define DESC_BLOCK_64B  (3u << 24)

/* version 0x10002 (address size in bits 25:24 = 2/3) or 0x10001 (0/1) */
#define SPIF_VER_V2     0x10002u
#define SPIF_VER_V1     0x10001u

#define NOR_WRSR        0x01
#define NOR_PP          0x02
#define NOR_RDSR        0x05
#define NOR_WREN        0x06
#define NOR_FAST_READ   0x0b
#define NOR_RDSR2       0x35
#define NOR_WRSR2       0x31
#define NOR_PP_QUAD     0x32
#define NOR_SE          0x20
#define NOR_FAST_READ_QUAD 0x6b
#define NOR_RDID        0x9f
#define NOR_BE64        0xd8
#define DTR_MODE_BYTE   0xff    /* no continuous read */
#define DTR_DUMMY_MAX   12

#define NOR_FAST_READ_4B 0x0c
#define NOR_PP_4B       0x12
#define NOR_SE_4B       0x21
#define NOR_FAST_READ_QUAD_4B 0x6c
#define NOR_PP_QUAD_4B  0x34
#define NOR_BE64_4B     0xdc

#define SR_WIP          RT_BIT(0)
#define SR_QE_SR1       RT_BIT(6)
#define SR2_QE          RT_BIT(1)
#define SR2_CMP         RT_BIT(6)

#define SPIF_SECTOR_SIZE     4096u
#define BLOCK_SIZE      65536u
#define PAGE_SIZE_NOR   256u
#define BOUNCE_SIZE     4096u
#define DESC_MAX_LEN    65536u          /* data of one descriptor */
#define DESC_COUNT      16u             /* descriptors of one command */
#define DIRECT_MAX      (DESC_COUNT * DESC_MAX_LEN)

#define IDENT_FREQ      24000000u       /* HOSC: no sample delay needed */
#define HOSC_HZ         24000000u
#define TUNE_DELAY_SETTLE_US 1000
#define TUNE_MIN_WINDOW 8u

/* the tuning stores its result in 32 byte records */
#define REC_MAGIC       "SPIFTUNE"
#define REC_SIZE        32u

struct spif_rec
{
    rt_uint8_t magic[8];
    rt_uint8_t jedec[3];
    rt_uint8_t mode;
    rt_uint32_t frequency;      /* the frequency the sample point is good for */
    rt_uint32_t requested;      /* the frequency asked for by the device tree */
    rt_uint8_t delay;
    rt_uint8_t dtr;             /* the sample point is for DTR reads */
    rt_uint8_t reserved[6];
    rt_uint32_t crc;
};
RT_STATIC_ASSERT(spif_rec_size, sizeof(struct spif_rec) == REC_SIZE);

struct spif_cmd
{
    rt_uint8_t opcode;
    rt_uint8_t addr_bytes;      /* 0, 3 or 4 */
    rt_uint32_t addr;
    rt_uint8_t dummy;           /* dummy clock cycles */
    rt_uint8_t addr_width;      /* 1, 2 or 4 wires */
    rt_uint8_t data_width;
    rt_bool_t mode;             /* a mode byte follows the address */
    rt_uint8_t mode_val;
    rt_uint8_t mode_width;
    rt_bool_t write;            /* data goes to the flash */
    rt_uint32_t len;            /* data bytes, in/from buf (default: the bounce buffer) */
    void *buf;                  /* 64 byte aligned, a multiple of 64 bytes long when set */
};

struct spif_xip_regs
{
    rt_uint32_t phc, tcf, tcs, tnm;
};

/* the DMA works on these: cache line aligned and a multiple of it */
struct spif_dma
{
    rt_uint8_t desc[DESC_COUNT * DMA_DESC_BYTES];
    rt_uint8_t buf[BOUNCE_SIZE];
    rt_uint8_t ref[BOUNCE_SIZE];
};

struct spif
{
    rt_ubase_t base;
    rt_ubase_t xip_base;
    rt_size_t xip_size;
    struct rt_clk *mod;
    struct rt_clk *bus;
    struct rt_mutex lock;
    struct spif_dma *dma;

    /* configuration (device tree) */
    rt_uint32_t frequency_cfg;
    rt_uint32_t size_cfg;       /* bytes, 0: from the JEDEC ID */
    rt_uint8_t jedec_cfg[3];
    rt_bool_t has_jedec;
    rt_bool_t quad_cfg;
    rt_bool_t dtr_cfg;
    rt_uint8_t dtr_opcode_cfg;
    rt_uint8_t dtr_dummy_cfg;   /* 0: probe */
    rt_bool_t addr_4byte_cfg;
    rt_bool_t clear_bp;
    rt_bool_t has_params;
    rt_uint32_t params_offset;
    rt_uint32_t tune_offset;
    rt_bool_t xip_at_init;
    rt_uint32_t xip_offset_cfg;
    rt_uint32_t xip_length_cfg;

    /* state */
    rt_uint32_t version;
    rt_uint8_t jedec[3];
    rt_uint32_t size;
    rt_bool_t addr_4byte;
    rt_bool_t quad;
    rt_bool_t dtr_cap;          /* DTR reads work (probed) */
    rt_bool_t dtr_on;           /* reads use DTR now */
    rt_bool_t dtr_active;       /* the controller is in DTR mode right now */
    rt_uint8_t dtr_dummy;
    rt_uint8_t dtr_opcode;
    rt_uint32_t frequency;
    rt_bool_t tuned;
    rt_uint8_t mode;
    rt_uint8_t delay;

    rt_bool_t xip;
    rt_uint32_t xip_offset;
    rt_uint32_t xip_length;
    struct spif_xip_regs xip_regs;
    rt_base_t irq_key;
    rt_bool_t xip_suspended;
    /* flash range written while the mapping was suspended */
    rt_uint32_t dirty_lo;
    rt_uint32_t dirty_hi;

    struct rt_mtd_nor_device mtd;
};

static struct spif *spif;

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SPIF_DIV_ROUND_UP(n, d) (((n) + (d) - 1u) / (d))

static rt_uint32_t spif_rd(rt_uint32_t reg)
{
    return HWREG32(spif->base + reg);
}

static void spif_wr(rt_uint32_t reg, rt_uint32_t val)
{
    HWREG32(spif->base + reg) = val;
}

static void spif_rmw(rt_uint32_t reg, rt_uint32_t clr, rt_uint32_t set)
{
    spif_wr(reg, (spif_rd(reg) & ~clr) | set);
}

static int spif_wait(rt_uint32_t reg, rt_uint32_t mask, rt_bool_t set, rt_uint32_t timeout_us)
{
    rt_uint32_t i, t;

    for (i = 0; i < 4096u; i++)
    {
        if (!!(spif_rd(reg) & mask) == set)
        {
            return 0;
        }
    }
    for (t = 0; t < timeout_us; t += 5u)
    {
        if (!!(spif_rd(reg) & mask) == set)
        {
            return 0;
        }
        rt_hw_us_delay(5);
    }

    return -RT_ETIMEOUT;
}

static rt_uint32_t crc32_ieee(const rt_uint8_t *p, rt_size_t n)
{
    rt_uint32_t crc = 0xffffffffu;
    rt_size_t i;
    int b;

    for (i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }

    return ~crc;
}

/* ---- clock ------------------------------------------------------------ */

/* module clock: 24 MHz or PLL_PERI_1X with the dividers the CCU picks; returns the rate */
static rt_uint32_t spif_set_clock(rt_uint32_t hz)
{
    rt_clk_set_rate(spif->mod, hz);

    return (rt_uint32_t)rt_clk_get_rate(spif->mod);
}

/* ---- controller ------------------------------------------------------- */

static void spif_fifo_reset(void)
{
    spif_rmw(SPIF_GCA, 0, GCA_FIFO_RST);
    (void)spif_wait(SPIF_GCA, GCA_FIFO_RST, RT_FALSE, 1000);
}

static void spif_soft_reset(void)
{
    spif_rmw(SPIF_GCA, 0, GCA_DMA_END);
    (void)spif_wait(SPIF_GCA, GCA_DMA_END, RT_FALSE, 1000);
    spif_rmw(SPIF_GCA, 0, GCA_SOFT_RST);
    (void)spif_wait(SPIF_GCA, GCA_SOFT_RST, RT_FALSE, 1000);
}

static void spif_set_tc(rt_bool_t enable, rt_uint8_t mode, rt_uint8_t delay)
{
    rt_uint32_t tc = spif_rd(SPIF_TC);

    tc &= ~(TC_SAMPLE_EN | TC_DELAY_SW_EN | TC_MODE_MASK | TC_DELAY_MASK);
    if (enable)
    {
        tc |= TC_SAMPLE_EN | TC_DELAY_SW_EN | ((rt_uint32_t)mode << TC_MODE_SHIFT) |
              ((rt_uint32_t)delay & TC_DELAY_MASK);
    }
    spif_wr(SPIF_TC, tc);
    /* the delay line needs a moment to settle */
    rt_hw_us_delay(TUNE_DELAY_SETTLE_US);
}

static void spif_hw_setup(void)
{
    spif_soft_reset();
    spif_fifo_reset();
    spif_rmw(SPIF_GC, GC_NMODE_EN | GC_PMODE_EN, 0);
    /* MSB first, no write protect/hold pins driven, chip select 0 active low, mode 0 */
    spif_rmw(SPIF_GC, GC_RX_FBS | GC_TX_FBS | GC_WP_EN | GC_HOLD_EN | GC_DTR_EN |
             GC_SS_MASK | GC_CPHA | GC_CPOL, GC_CS_POL);
    spif_rmw(SPIF_TC, TC_SCKOUT_SEL, 0);
    spif_wr(SPIF_CSD, CSD_DEFAULT);
    spif_wr(SPIF_INT_EN, 0);
    spif_wr(SPIF_INT_STA, 0xffffffffu);
}

static rt_uint32_t width_code(rt_uint8_t width)
{
    return width == 4u ? 2u : (width == 2u ? 1u : 0u);
}

/* the four words that describe a command */
static void spif_build(const struct spif_cmd *c, rt_uint32_t *phc, rt_uint32_t *tcf,
                       rt_uint32_t *tcs, rt_uint32_t *tnm)
{
    *phc = PHC_CMD;
    *tcf = 0;
    *tcs = ((rt_uint32_t)c->opcode << TCS_OPCODE_SHIFT);
    *tnm = TNM_NORMAL_EN;

    if (c->addr_bytes != 0u)
    {
        rt_uint32_t size;

        *phc |= PHC_ADDR;
        *tcf = c->addr;
        *tcs |= width_code(c->addr_width) << TCS_ADDR_SHIFT;
        if (spif->version >= SPIF_VER_V2)
        {
            size = c->addr_bytes == 4u ? 3u : 2u;
        }
        else
        {
            size = c->addr_bytes == 4u ? 1u : 0u;
        }
        *tnm |= size << 24;
    }
    if (c->mode)
    {
        *phc |= PHC_MODE;
        *tcs |= ((rt_uint32_t)c->mode_val << TCS_MODE_OPCODE_SHIFT) |
                (width_code(c->mode_width) << TCS_MODE_SHIFT);
    }
    if (c->dummy != 0u)
    {
        *phc |= PHC_DUMMY;
        *tnm |= (rt_uint32_t)c->dummy << TNM_DUMMY_SHIFT;
    }
    if (c->len != 0u)
    {
        *phc |= c->write ? PHC_TX : PHC_RX;
        *tcs |= width_code(c->data_width) << TCS_DATA_SHIFT;
        *tnm |= c->len >= 65536u ? TNM_LEN_64K : c->len;
    }
}

static int spif_recover(void)
{
    rt_uint32_t tc = spif_rd(SPIF_TC);

    spif_hw_setup();
    spif_wr(SPIF_TC, tc);
    spif->dtr_active = RT_FALSE;
    spif_set_clock(spif->frequency);

    return -RT_ETIMEOUT;
}

/* Run one command. Data is read to or written from dma->buf. */
static int spif_xfer(const struct spif_cmd *c)
{
    struct spif_dma *dma = spif->dma;
    rt_uint32_t phc, tcf, tcs, tnm;
    rt_uint32_t *desc;
    rt_uint8_t *buf;
    rt_uint32_t blen, ndesc, i;

    spif_build(c, &phc, &tcf, &tcs, &tnm);
    spif_fifo_reset();

    if (c->len == 0u)
    {
        spif_rmw(SPIF_GC, GC_DMA_MODE, 0);
        spif_wr(SPIF_PHC, phc);
        spif_wr(SPIF_TCF, tcf);
        spif_wr(SPIF_TCS, tcs);
        spif_wr(SPIF_TNM, tnm);
        spif_rmw(SPIF_GC, 0, GC_NMODE_EN);
        if (spif_wait(SPIF_GC, GC_NMODE_EN, RT_FALSE, 100000) != 0)
        {
            rt_kprintf("spif: command %02x timed out\n", c->opcode);
            return spif_recover();
        }

        return 0;
    }

    desc = (rt_uint32_t *)dma->desc;
    buf = c->buf != RT_NULL ? c->buf : dma->buf;
    blen = RT_ALIGN(c->len, 64u);
    ndesc = SPIF_DIV_ROUND_UP(c->len, DESC_MAX_LEN);

    if (ndesc > DESC_COUNT)
    {
        return -RT_EINVAL;
    }
    /* a chain of descriptors, each moves up to 64 KiB with the same command */
    for (i = 0; i < ndesc; i++)
    {
        rt_uint32_t *w = &desc[i * 8u];
        rt_uint32_t n = MIN(c->len - i * DESC_MAX_LEN, DESC_MAX_LEN);
        rt_bool_t last = i == ndesc - 1u;

        w[0] = DESC_BURST_INCR16 | (last ? DESC_LAST : 0u) | (c->write ? 0u : DESC_READ);
        w[1] = DESC_BLOCK_64B | n;
        w[2] = (rt_uint32_t)(rt_ubase_t)(buf + i * DESC_MAX_LEN) >> 2;
        w[3] = last ? 0u : (rt_uint32_t)(rt_ubase_t)&desc[(i + 1u) * 8u] >> 2;
        w[4] = phc;
        w[5] = tcf + i * DESC_MAX_LEN;
        w[6] = tcs;
        w[7] = (tnm & ~(TNM_LEN_64K | 0xffffu)) | (n == DESC_MAX_LEN ? TNM_LEN_64K : n);
    }

    sunxi_dcache_flush(dma->desc, ndesc * DMA_DESC_BYTES);
    if (c->write)
    {
        sunxi_dcache_flush(buf, blen);
    }
    else
    {
        sunxi_dcache_flush_inval(buf, blen);
    }

    spif_rmw(SPIF_GC, 0, GC_DMA_MODE);
    spif_wr(SPIF_INT_STA, INT_DMA_DONE | INT_ERR);
    spif_wr(SPIF_DSC, (rt_uint32_t)(rt_ubase_t)dma->desc >> 2);
    spif_rmw(SPIF_DMA_CTL, DMA_CTL_DESC_LEN_MASK, DMA_DESC_BYTES << 4);
    spif_rmw(SPIF_DMA_CTL, 0, DMA_CTL_START);

    if (spif_wait(SPIF_INT_STA, INT_DMA_DONE | INT_ERR, RT_TRUE, 200000u + ndesc * 20000u) != 0)
    {
        rt_kprintf("spif: DMA of command %02x timed out\n", c->opcode);
        return spif_recover();
    }
    if ((spif_rd(SPIF_INT_STA) & INT_ERR) != 0u)
    {
        rt_kprintf("spif: DMA error %08x\n", spif_rd(SPIF_INT_STA));
        spif_wr(SPIF_INT_STA, INT_DMA_DONE | INT_ERR);
        spif_recover();

        return -RT_EIO;
    }
    spif_wr(SPIF_INT_STA, INT_DMA_DONE);
    if (!c->write)
    {
        sunxi_dcache_inval(buf, blen);
    }

    return 0;
}

/*
 * Double data rate: address and data change on both clock edges, the module
 * clock runs at twice the SCK. Only the read command uses it.
 */
static void spif_dtr_set(rt_bool_t on)
{
    if (on == spif->dtr_active)
    {
        return;
    }
    spif_rmw(SPIF_TC, TC_SCKOUT_SEL, on ? TC_SCKOUT_SEL : 0);
    spif_rmw(SPIF_GC, GC_DTR_EN, on ? GC_DTR_EN : 0);
    spif_set_clock(on ? 2u * spif->frequency : spif->frequency);
    spif_soft_reset();
    spif->dtr_active = on;
}

/* the read command in use: DTR 1-4-4 with a mode byte, 1-1-4 or one wire */
static void spif_read_cmd(rt_uint32_t off, rt_uint32_t len, struct spif_cmd *c)
{
    memset(c, 0, sizeof(*c));
    c->addr_bytes = spif->addr_4byte ? 4u : 3u;
    c->addr = off;
    c->len = len;
    c->addr_width = 1;
    if (spif->dtr_on)
    {
        c->opcode = spif->dtr_opcode;
        c->addr_width = 4;
        c->data_width = 4;
        c->mode = RT_TRUE;
        c->mode_val = DTR_MODE_BYTE;
        c->mode_width = 4;
        c->dummy = spif->dtr_dummy;
        return;
    }
    c->dummy = 8;
    c->data_width = spif->quad ? 4u : 1u;
    c->opcode = spif->quad ? (spif->addr_4byte ? NOR_FAST_READ_QUAD_4B : NOR_FAST_READ_QUAD)
                           : (spif->addr_4byte ? NOR_FAST_READ_4B : NOR_FAST_READ);
}

/* ---- XIP mapping ------------------------------------------------------ */

static void spif_window_regs(struct spif_xip_regs *r)
{
    struct spif_cmd c;

    spif_read_cmd(spif->xip_offset, 1, &c);
    spif_build(&c, &r->phc, &r->tcf, &r->tcs, &r->tnm);
    /* a window read is described without the descriptor enable bit */
    r->tnm &= ~(TNM_NORMAL_EN | 0xffffu);
}

static void spif_window_on(const struct spif_xip_regs *r)
{
    spif_dtr_set(spif->dtr_on);
    spif_rmw(SPIF_GC, GC_NMODE_EN | GC_PMODE_EN | GC_DMA_MODE, 0);
    spif_wr(SPIF_PSA, (rt_uint32_t)spif->xip_base);
    spif_wr(SPIF_PEA, (rt_uint32_t)spif->xip_base + spif->xip_length);
    spif_rmw(SPIF_GC, 0, GC_ADDR_MAP);
    spif_wr(SPIF_PMA, spif->xip_offset);
    spif_wr(SPIF_PHC, r->phc);
    spif_wr(SPIF_TCF, r->tcf);
    spif_wr(SPIF_TCS, r->tcs);
    spif_wr(SPIF_TNM, r->tnm);
    spif_rmw(SPIF_GC, 0, GC_PMODE_EN);
}

static void spif_window_off(void)
{
    spif_dtr_set(RT_FALSE);
    spif_soft_reset();
    spif_fifo_reset();
    spif_rmw(SPIF_GC, GC_NMODE_EN | GC_PMODE_EN | GC_ADDR_MAP, 0);
    spif_wr(SPIF_PMA, 0);
}

/*
 * Bracket every access to the flash: with a mapping active it is taken down
 * for the duration of the command, with interrupts masked so that nothing
 * can run from (or read) the window meanwhile.
 */
static void spif_begin(void)
{
    if (!spif->xip)
    {
        return;
    }
    spif->irq_key = rt_hw_interrupt_disable();
    spif->xip_suspended = RT_TRUE;
    spif_window_off();
}

static void spif_end(void)
{
    if (!spif->xip)
    {
        return;
    }
    spif_soft_reset();
    spif_window_on(&spif->xip_regs);
    if (spif->dirty_hi > spif->dirty_lo)
    {
        rt_uint32_t lo = MAX(spif->dirty_lo, spif->xip_offset);
        rt_uint32_t hi = MIN(spif->dirty_hi, spif->xip_offset + spif->xip_length);

        if (hi > lo)
        {
            rt_ubase_t start = spif->xip_base + (lo - spif->xip_offset);

            sunxi_dcache_inval((void *)start, hi - lo);
            rt_hw_cpu_icache_ops(RT_HW_CACHE_INVALIDATE, (void *)start, hi - lo);
        }
    }
    spif->dirty_lo = 0xffffffffu;
    spif->dirty_hi = 0;
    spif->xip_suspended = RT_FALSE;
    rt_hw_interrupt_enable(spif->irq_key);
}

static void spif_mark_dirty(rt_uint32_t off, rt_uint32_t len)
{
    spif->dirty_lo = MIN(spif->dirty_lo, off);
    spif->dirty_hi = MAX(spif->dirty_hi, off + len);
}

/* ---- NOR commands ----------------------------------------------------- */

static int nor_simple(rt_uint8_t opcode, rt_uint32_t rx_len, const rt_uint8_t *tx, rt_uint32_t tx_len)
{
    struct spif_cmd c;

    memset(&c, 0, sizeof(c));
    c.opcode = opcode;
    c.data_width = 1;
    if (rx_len != 0u)
    {
        c.len = rx_len;
    }
    else if (tx_len != 0u)
    {
        c.len = tx_len;
        c.write = RT_TRUE;
        memcpy(spif->dma->buf, tx, tx_len);
    }

    return spif_xfer(&c);
}

static int nor_read_status(rt_uint8_t opcode, rt_uint8_t *sr)
{
    int ret = nor_simple(opcode, 1, RT_NULL, 0);

    if (ret == 0)
    {
        *sr = spif->dma->buf[0];
    }

    return ret;
}

static int nor_write_enable(void)
{
    return nor_simple(NOR_WREN, 0, RT_NULL, 0);
}

/* poll the work in progress bit; the caller holds the bracket */
static int nor_wait_ready(rt_uint32_t timeout_ms)
{
    rt_tick_t end = rt_tick_get_millisecond() + timeout_ms;
    rt_uint8_t sr;
    int ret;

    do
    {
        ret = nor_read_status(NOR_RDSR, &sr);
        if (ret != 0)
        {
            return ret;
        }
        if ((sr & SR_WIP) == 0u)
        {
            return 0;
        }
        rt_hw_us_delay(spif->xip_suspended ? 100 : 20);
    } while ((rt_int32_t)(end - rt_tick_get_millisecond()) > 0);

    return -RT_ETIMEOUT;
}

static int nor_enable_quad(void)
{
    rt_uint8_t mfr = spif->jedec[0];
    rt_uint8_t sr, sr2, both[2];
    int ret;

    if (mfr == 0xc2 || mfr == 0x9d)
    {
        /* Macronix, ISSI: quad enable is bit 6 of status register 1 */
        ret = nor_read_status(NOR_RDSR, &sr);
        if (ret != 0)
        {
            return ret;
        }
        if ((sr & SR_QE_SR1) == 0u)
        {
            sr |= SR_QE_SR1;
            if (nor_write_enable() != 0 || nor_simple(NOR_WRSR, 0, &sr, 1) != 0 ||
                nor_wait_ready(100) != 0)
            {
                return -RT_EIO;
            }
            ret = nor_read_status(NOR_RDSR, &sr);
            if (ret != 0 || (sr & SR_QE_SR1) == 0u)
            {
                return -RT_EIO;
            }
        }

        return 0;
    }

    /* the others keep it in bit 1 of status register 2 */
    ret = nor_read_status(NOR_RDSR2, &sr2);
    if (ret != 0)
    {
        return ret;
    }
    if ((sr2 & SR2_QE) != 0u)
    {
        return 0;
    }
    sr2 |= SR2_QE;
    if (nor_write_enable() != 0 || nor_simple(NOR_WRSR2, 0, &sr2, 1) != 0 || nor_wait_ready(100) != 0)
    {
        return -RT_EIO;
    }
    ret = nor_read_status(NOR_RDSR2, &sr2);
    if (ret == 0 && (sr2 & SR2_QE) != 0u)
    {
        return 0;
    }

    /* parts that take both status registers with one write command */
    if (nor_read_status(NOR_RDSR, &both[0]) != 0)
    {
        return -RT_EIO;
    }
    both[1] = sr2 | SR2_QE;
    if (nor_write_enable() != 0 || nor_simple(NOR_WRSR, 0, both, 2) != 0 || nor_wait_ready(100) != 0)
    {
        return -RT_EIO;
    }
    ret = nor_read_status(NOR_RDSR2, &sr2);

    return (ret == 0 && (sr2 & SR2_QE) != 0u) ? 0 : -RT_EIO;
}

/*
 * Block protect bits: BP0..BP2, TB and SEC in bits 2..6 of status register 1
 * (BP0..BP3 in 2..5 and the quad enable in bit 6 for Macronix and ISSI), and
 * CMP in bit 6 of status register 2. Status register 1 and 2 are written
 * together so that no part resets the quad enable bit.
 */
static int nor_clear_protect(rt_uint8_t *sr1_out)
{
    rt_bool_t sr1_qe = spif->jedec[0] == 0xc2 || spif->jedec[0] == 0x9d;
    rt_uint8_t mask = sr1_qe ? 0x3c : 0x7c;
    rt_uint8_t sr1, sr2 = 0, wr[2];
    int ret = nor_read_status(NOR_RDSR, &sr1);

    if (ret == 0 && !sr1_qe)
    {
        ret = nor_read_status(NOR_RDSR2, &sr2);
    }
    if (ret != 0)
    {
        return ret;
    }
    *sr1_out = sr1;
    if ((sr1 & mask) == 0u && (sr2 & SR2_CMP) == 0u)
    {
        return 0;
    }
    wr[0] = sr1 & ~mask;
    wr[1] = sr2 & ~SR2_CMP;
    ret = nor_write_enable();
    if (ret == 0)
    {
        ret = nor_simple(NOR_WRSR, 0, wr, sr1_qe ? 1 : 2);
    }
    if (ret == 0)
    {
        ret = nor_wait_ready(500);
    }
    if (ret == 0)
    {
        ret = nor_read_status(NOR_RDSR, &sr1);
    }
    if (ret == 0 && (sr1 & mask) != 0u)
    {
        return -RT_ERROR;
    }
    *sr1_out = sr1;

    return ret;
}

/* read into buf (NULL: the bounce buffer, up to BOUNCE_SIZE bytes) */
static int nor_read_to(rt_uint32_t off, void *buf, rt_uint32_t len)
{
    struct spif_cmd c;
    int ret;

    spif_read_cmd(off, len, &c);
    c.buf = buf;
    spif_begin();
    spif_dtr_set(spif->dtr_on);
    ret = spif_xfer(&c);
    spif_dtr_set(RT_FALSE);
    spif_end();

    return ret;
}

static int nor_read_chunk(rt_uint32_t off, rt_uint32_t len)
{
    return nor_read_to(off, RT_NULL, len);
}

static int nor_program_page(rt_uint32_t off, rt_uint32_t len)
{
    struct spif_cmd c;
    int ret;

    memset(&c, 0, sizeof(c));
    c.opcode = spif->quad ? (spif->addr_4byte ? NOR_PP_QUAD_4B : NOR_PP_QUAD)
                          : (spif->addr_4byte ? NOR_PP_4B : NOR_PP);
    c.addr_bytes = spif->addr_4byte ? 4u : 3u;
    c.addr = off;
    c.addr_width = 1;
    c.data_width = spif->quad ? 4u : 1u;
    c.write = RT_TRUE;
    c.len = len;

    spif_begin();
    ret = nor_write_enable();
    if (ret == 0)
    {
        ret = spif_xfer(&c);
    }
    if (ret == 0)
    {
        ret = nor_wait_ready(50);
        spif_mark_dirty(off, len);
    }
    spif_end();

    return ret;
}

static int nor_erase_block(rt_uint32_t off, rt_bool_t big)
{
    struct spif_cmd c;
    int ret;

    memset(&c, 0, sizeof(c));
    c.opcode = big ? (spif->addr_4byte ? NOR_BE64_4B : NOR_BE64)
                   : (spif->addr_4byte ? NOR_SE_4B : NOR_SE);
    c.addr_bytes = spif->addr_4byte ? 4u : 3u;
    c.addr = off;
    c.addr_width = 1;

    spif_begin();
    ret = nor_write_enable();
    if (ret == 0)
    {
        ret = spif_xfer(&c);
    }
    if (ret == 0)
    {
        ret = nor_wait_ready(big ? 3000 : 1000);
        spif_mark_dirty(off, big ? BLOCK_SIZE : SPIF_SECTOR_SIZE);
    }
    spif_end();

    return ret;
}

/* ---- flash API -------------------------------------------------------- */

static int spif_check_range(rt_uint64_t off, rt_size_t len)
{
    if (!spif || off + len > spif->size)
    {
        return -RT_EINVAL;
    }

    return 0;
}

rt_ssize_t sun252i_spif_read(rt_uint32_t off, void *dst, rt_size_t len)
{
    rt_uint8_t *out = dst;
    rt_size_t total = len;
    int ret = spif_check_range(off, len);

    if (ret != 0)
    {
        return ret;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    while (len != 0u && ret == 0)
    {
        rt_uint32_t n;

        if (((rt_ubase_t)out % 64u) == 0u && len >= 64u)
        {
            /* straight into the caller's buffer, a whole number of cache lines */
            n = MIN(RT_ALIGN_DOWN(len, 64u), DIRECT_MAX);
            ret = nor_read_to(off, out, n);
        }
        else
        {
            n = MIN(len, BOUNCE_SIZE);
            ret = nor_read_chunk(off, n);
            if (ret == 0)
            {
                memcpy(out, spif->dma->buf, n);
            }
        }
        if (ret == 0)
        {
            off += n;
            out += n;
            len -= n;
        }
    }
    rt_mutex_release(&spif->lock);

    return ret == 0 ? (rt_ssize_t)total : ret;
}

rt_ssize_t sun252i_spif_write(rt_uint32_t off, const void *src, rt_size_t len)
{
    const rt_uint8_t *in = src;
    rt_size_t total = len;
    int ret = spif_check_range(off, len);

    if (ret != 0)
    {
        return ret;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    while (len != 0u && ret == 0)
    {
        rt_uint32_t n = MIN(len, PAGE_SIZE_NOR - (off % PAGE_SIZE_NOR));

        memcpy(spif->dma->buf, in, n);
        ret = nor_program_page(off, n);
        off += n;
        in += n;
        len -= n;
    }
    rt_mutex_release(&spif->lock);

    return ret == 0 ? (rt_ssize_t)total : ret;
}

rt_err_t sun252i_spif_erase(rt_uint32_t off, rt_size_t len)
{
    int ret = spif_check_range(off, len);

    if (ret != 0)
    {
        return ret;
    }
    if ((off % SPIF_SECTOR_SIZE) != 0 || (len % SPIF_SECTOR_SIZE) != 0u)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    while (len != 0u && ret == 0)
    {
        rt_bool_t big = (off % BLOCK_SIZE) == 0 && len >= BLOCK_SIZE;
        rt_uint32_t n = big ? BLOCK_SIZE : SPIF_SECTOR_SIZE;

        ret = nor_erase_block(off, big);
        off += n;
        len -= n;
    }
    rt_mutex_release(&spif->lock);

    return ret;
}

/* ---- tuning and stored parameters ------------------------------------ */

static void spif_apply_op_point(rt_uint32_t hz, rt_bool_t tuned, rt_uint8_t mode, rt_uint8_t delay)
{
    spif->frequency = spif_set_clock(hz);
    spif_set_tc(tuned, mode, delay);
    spif->tuned = tuned;
    spif->dtr_on = tuned && spif->dtr_cap;
    spif->mode = mode;
    spif->delay = delay;
}

/* the operating point is saved around work that has to run at the safe one */
struct spif_point
{
    rt_uint32_t hz;
    rt_bool_t tuned;
    rt_uint8_t mode, delay;
};

static void spif_point_save(struct spif_point *p)
{
    p->hz = spif->frequency;
    p->tuned = spif->tuned;
    p->mode = spif->mode;
    p->delay = spif->delay;
}

static void spif_point_restore(const struct spif_point *p)
{
    spif_apply_op_point(p->hz, p->tuned, p->mode, p->delay);
}

rt_err_t sun252i_spif_set_sample(rt_uint8_t mode, rt_uint8_t delay)
{
    if (!spif || mode >= SUN252I_SPIF_TUNE_MODES || delay >= SUN252I_SPIF_TUNE_DELAYS)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    if (spif->xip)
    {
        rt_mutex_release(&spif->lock);
        return -RT_EBUSY;
    }
    spif_apply_op_point(spif->frequency_cfg, RT_TRUE, mode, delay);
    rt_mutex_release(&spif->lock);

    return RT_EOK;
}

static rt_bool_t spif_data_is_usable(const rt_uint8_t *p, rt_size_t n)
{
    rt_uint32_t seen[8] = { 0 };
    rt_uint32_t distinct = 0;
    rt_size_t i;

    for (i = 0; i < n; i++)
    {
        rt_uint32_t *w = &seen[p[i] / 32u];
        rt_uint32_t bit = RT_BIT(p[i] % 32u);

        if ((*w & bit) == 0u)
        {
            *w |= bit;
            distinct++;
        }
    }

    return distinct >= 32u;
}

/* find out whether DTR reads work, and with how many dummy cycles */
static void spif_dtr_probe(void)
{
    rt_uint32_t off = spif->tune_offset;
    rt_uint8_t first = spif->dtr_dummy_cfg != 0u ? spif->dtr_dummy_cfg : 1u;
    rt_uint8_t last = spif->dtr_dummy_cfg != 0u ? spif->dtr_dummy_cfg : DTR_DUMMY_MAX;
    rt_uint8_t n;

    spif->dtr_cap = RT_FALSE;
    spif->dtr_on = RT_FALSE;
    if (!spif->dtr_cfg || !spif->quad || spif->addr_4byte || off + BOUNCE_SIZE > spif->size)
    {
        return;
    }
    if (nor_read_chunk(off, BOUNCE_SIZE) != 0)
    {
        return;
    }
    memcpy(spif->dma->ref, spif->dma->buf, BOUNCE_SIZE);
    if (!spif_data_is_usable(spif->dma->ref, BOUNCE_SIZE))
    {
        rt_kprintf("spif: tune area holds no usable data, DTR not probed\n");
        return;
    }
    spif->dtr_opcode = spif->dtr_opcode_cfg;
    spif->dtr_on = RT_TRUE;
    for (n = first; n <= last; n++)
    {
        spif->dtr_dummy = n;
        memset(spif->dma->buf, 0, BOUNCE_SIZE);
        if (nor_read_chunk(off, BOUNCE_SIZE) == 0 &&
            memcmp(spif->dma->buf, spif->dma->ref, BOUNCE_SIZE) == 0)
        {
            spif->dtr_cap = RT_TRUE;
            rt_kprintf("spif: DTR read %02x works with %u dummy cycles\n", spif->dtr_opcode, n);
            break;
        }
    }
    spif->dtr_on = RT_FALSE;
    if (!spif->dtr_cap)
    {
        rt_kprintf("spif: DTR read %02x does not work, using SDR\n", spif->dtr_opcode_cfg);
    }
}

/* the one wire command the points are checked with, besides the data read */
static rt_bool_t spif_id_ok(void)
{
    return nor_simple(NOR_RDID, 3, RT_NULL, 0) == 0 && memcmp(spif->dma->buf, spif->jedec, 3) == 0;
}

/*
 * Try every sample mode and delay at the current frequency. A point is good
 * when the JEDEC ID (one wire) and the reference data (the read mode in use)
 * both come back right, twice. Returns the width of the widest window.
 */
static rt_uint32_t spif_scan(rt_uint32_t off, struct sun252i_spif_tune_result *r,
                             rt_uint8_t *best_mode, rt_uint8_t *best_start)
{
    rt_uint32_t best_len = 0;
    rt_uint8_t mode, delay;

    memset(r->ok, 0, sizeof(r->ok));
    memset(r->ok_one_wire, 0, sizeof(r->ok_one_wire));
    for (mode = 0; mode < SUN252I_SPIF_TUNE_MODES; mode++)
    {
        rt_uint32_t run = 0;

        for (delay = 0; delay < SUN252I_SPIF_TUNE_DELAYS; delay++)
        {
            rt_bool_t one = RT_TRUE, data = RT_TRUE;
            int pass;

            spif_set_tc(RT_TRUE, mode, delay);
            for (pass = 0; pass < 2; pass++)
            {
                spif_begin();
                one = one && spif_id_ok();
                spif_end();
                memset(spif->dma->buf, 0, BOUNCE_SIZE);
                data = data && nor_read_chunk(off, BOUNCE_SIZE) == 0 &&
                       memcmp(spif->dma->buf, spif->dma->ref, BOUNCE_SIZE) == 0;
            }
            if (one)
            {
                r->ok_one_wire[mode] |= (rt_uint64_t)1 << delay;
            }
            if (one && data)
            {
                r->ok[mode] |= (rt_uint64_t)1 << delay;
                run++;
                if (run > best_len)
                {
                    best_len = run;
                    *best_start = delay + 1u - run;
                    *best_mode = mode;
                }
            }
            else
            {
                run = 0;
            }
        }
    }

    return best_len;
}

/* frequencies tried in turn when no window is found at the requested one */
static const rt_uint32_t tune_fallback_hz[] = { 75000000, 60000000, 50000000, 40000000, 30000000 };

rt_err_t sun252i_spif_tune(struct sun252i_spif_tune_result *res)
{
    struct sun252i_spif_tune_result r;
    rt_uint32_t off;
    rt_uint8_t best_mode = 0, best_start = 0;
    rt_uint32_t best_len;
    int ret, attempt, i;

    memset(&r, 0, sizeof(r));
    if (!spif)
    {
        return -RT_ERROR;
    }
    off = spif->tune_offset;
    if (off + BOUNCE_SIZE > spif->size)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    if (spif->xip)
    {
        rt_mutex_release(&spif->lock);
        return -RT_EBUSY;
    }

    /* the reference is read at a frequency that needs no tuning */
    spif_apply_op_point(IDENT_FREQ, RT_FALSE, 0, 0);
    ret = nor_read_chunk(off, BOUNCE_SIZE);
    if (ret != 0)
    {
        goto out;
    }
    memcpy(spif->dma->ref, spif->dma->buf, BOUNCE_SIZE);
    if (!spif_data_is_usable(spif->dma->ref, BOUNCE_SIZE))
    {
        ret = -RT_ENOSYS;
        goto out;
    }

    ret = -RT_EIO;
    for (attempt = 0; attempt < 2 && ret != 0; attempt++)
    {
        if (attempt == 1)
        {
            if (!spif->dtr_cap)
            {
                break;
            }
            rt_kprintf("spif: no sample point for DTR reads, using SDR\n");
            spif->dtr_cap = RT_FALSE;
        }
        for (i = -1; i < (int)RT_ARRAY_SIZE(tune_fallback_hz); i++)
        {
            rt_uint32_t hz = i < 0 ? spif->frequency_cfg : tune_fallback_hz[i];

            if (hz > spif->frequency_cfg || hz <= IDENT_FREQ)
            {
                continue;
            }
            spif->frequency = spif_set_clock(hz);
            spif->dtr_on = spif->dtr_cap;
            r.frequency = spif->frequency;
            best_len = spif_scan(off, &r, &best_mode, &best_start);
            if (best_len >= TUNE_MIN_WINDOW)
            {
                r.mode = best_mode;
                r.window_start = best_start;
                r.window_len = best_len;
                r.delay = best_start + best_len / 2u;
                spif_apply_op_point(hz, RT_TRUE, r.mode, r.delay);
                ret = 0;
                break;
            }
        }
    }
    r.dtr = spif->dtr_cap;
    if (ret != 0)
    {
        spif_apply_op_point(IDENT_FREQ, RT_FALSE, 0, 0);
    }
out:
    rt_mutex_release(&spif->lock);
    if (res != RT_NULL)
    {
        *res = r;
    }

    return ret;
}

static void spif_rec_make(struct spif_rec *rec)
{
    memset(rec, 0, sizeof(*rec));
    memcpy(rec->magic, REC_MAGIC, sizeof(rec->magic));
    memcpy(rec->jedec, spif->jedec, 3);
    rec->mode = spif->mode;
    rec->frequency = spif->frequency;
    rec->requested = spif->frequency_cfg;
    rec->delay = spif->delay;
    rec->dtr = spif->dtr_cap;
    rec->crc = crc32_ieee((const rt_uint8_t *)rec, offsetof(struct spif_rec, crc));
}

static rt_bool_t spif_rec_valid(const struct spif_rec *rec)
{
    return memcmp(rec->magic, REC_MAGIC, sizeof(rec->magic)) == 0 &&
           rec->crc == crc32_ieee((const rt_uint8_t *)rec, offsetof(struct spif_rec, crc)) &&
           memcmp(rec->jedec, spif->jedec, 3) == 0 && rec->requested == spif->frequency_cfg &&
           rec->frequency > IDENT_FREQ && rec->frequency <= spif->frequency_cfg &&
           rec->mode < SUN252I_SPIF_TUNE_MODES && rec->delay < SUN252I_SPIF_TUNE_DELAYS &&
           rec->dtr == spif->dtr_cap;
}

static rt_bool_t spif_rec_blank(const struct spif_rec *rec)
{
    const rt_uint8_t *p = (const rt_uint8_t *)rec;
    rt_size_t i;

    for (i = 0; i < sizeof(*rec); i++)
    {
        if (p[i] != 0xff)
        {
            return RT_FALSE;
        }
    }

    return RT_TRUE;
}

/*
 * Read the params sector at the safe operating point (the tuned one might be
 * the very thing that is wrong) and find the last valid record and the first
 * blank slot; -1 when there is none. The sector is left in the bounce buffer.
 */
static int spif_params_scan(int *last_valid, int *first_blank)
{
    int i, ret = nor_read_chunk(spif->params_offset, SPIF_SECTOR_SIZE);

    if (ret != 0)
    {
        return ret;
    }
    *last_valid = -1;
    *first_blank = -1;
    for (i = 0; i < (int)(SPIF_SECTOR_SIZE / REC_SIZE); i++)
    {
        const struct spif_rec *rec = (const struct spif_rec *)&spif->dma->buf[i * REC_SIZE];

        if (spif_rec_blank(rec))
        {
            *first_blank = i;
            break;
        }
        if (spif_rec_valid(rec))
        {
            *last_valid = i;
        }
    }

    return 0;
}

rt_err_t sun252i_spif_params_load(void)
{
    struct spif_point saved;
    struct spif_rec rec;
    int last, blank, ret;

    if (!spif || !spif->has_params)
    {
        return -RT_ENOSYS;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    if (spif->xip)
    {
        rt_mutex_release(&spif->lock);
        return -RT_EBUSY;
    }
    spif_point_save(&saved);
    spif_apply_op_point(IDENT_FREQ, RT_FALSE, 0, 0);
    ret = spif_params_scan(&last, &blank);
    if (ret == 0 && last < 0)
    {
        ret = -RT_EEMPTY;
    }
    if (ret == 0)
    {
        memcpy(&rec, &spif->dma->buf[last * REC_SIZE], sizeof(rec));
        spif_apply_op_point(rec.frequency, RT_TRUE, rec.mode, rec.delay);
    }
    else
    {
        spif_point_restore(&saved);
    }
    rt_mutex_release(&spif->lock);

    return ret;
}

rt_err_t sun252i_spif_params_save(void)
{
    struct spif_point saved;
    struct spif_rec rec;
    int last, blank, ret;

    if (!spif || !spif->has_params)
    {
        return -RT_ENOSYS;
    }
    if (!spif->tuned)
    {
        return -RT_EBUSY;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    spif_point_save(&saved);
    spif_rec_make(&rec);
    /* with the window mapped the clock stays where it is */
    if (!spif->xip)
    {
        spif_apply_op_point(IDENT_FREQ, RT_FALSE, 0, 0);
    }
    ret = spif_params_scan(&last, &blank);
    if (ret != 0)
    {
        goto out;
    }
    if (last >= 0 && memcmp(&spif->dma->buf[last * REC_SIZE], &rec, sizeof(rec)) == 0)
    {
        /* what is stored is what is applied */
        goto out;
    }
    if (blank < 0)
    {
        ret = nor_erase_block(spif->params_offset, RT_FALSE);
        blank = 0;
    }
    if (ret == 0)
    {
        memcpy(spif->dma->buf, &rec, sizeof(rec));
        ret = nor_program_page(spif->params_offset + blank * REC_SIZE, sizeof(rec));
    }
    if (ret == 0)
    {
        /* a protected flash ignores the program command silently */
        ret = nor_read_chunk(spif->params_offset + blank * REC_SIZE, sizeof(rec));
        if (ret == 0 && memcmp(spif->dma->buf, &rec, sizeof(rec)) != 0)
        {
            ret = -RT_EIO;
        }
    }
out:
    if (!spif->xip)
    {
        spif_point_restore(&saved);
    }
    rt_mutex_release(&spif->lock);

    return ret;
}

rt_err_t sun252i_spif_params_erase(void)
{
    struct spif_point saved;
    int ret;

    if (!spif || !spif->has_params)
    {
        return -RT_ENOSYS;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    spif_point_save(&saved);
    if (!spif->xip)
    {
        spif_apply_op_point(IDENT_FREQ, RT_FALSE, 0, 0);
    }
    ret = nor_erase_block(spif->params_offset, RT_FALSE);
    if (!spif->xip)
    {
        spif_point_restore(&saved);
    }
    rt_mutex_release(&spif->lock);

    return ret;
}

/* ---- XIP -------------------------------------------------------------- */

rt_err_t sun252i_spif_xip_enable(rt_uint32_t flash_offset, rt_size_t length)
{
    int ret = 0;

    if (!spif || (flash_offset % SPIF_SECTOR_SIZE) != 0u || length == 0u || length > spif->xip_size ||
        (rt_uint64_t)flash_offset + length > spif->size)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    if (spif->xip)
    {
        ret = -RT_EBUSY;
        goto out;
    }
    spif->xip_offset = flash_offset;
    spif->xip_length = length;
    spif_window_regs(&spif->xip_regs);
    spif->dirty_lo = 0xffffffffu;
    spif->dirty_hi = 0;
    /* nothing of the window can be in the caches yet, but be sure */
    sunxi_dcache_inval((void *)spif->xip_base, length);
    rt_hw_cpu_icache_ops(RT_HW_CACHE_INVALIDATE, (void *)spif->xip_base, length);
    spif_soft_reset();
    spif_fifo_reset();
    spif_window_on(&spif->xip_regs);
    spif->xip = RT_TRUE;
out:
    rt_mutex_release(&spif->lock);

    return ret;
}

rt_err_t sun252i_spif_xip_disable(void)
{
    if (!spif)
    {
        return -RT_ERROR;
    }
    rt_mutex_take(&spif->lock, RT_WAITING_FOREVER);
    if (spif->xip)
    {
        spif_window_off();
        sunxi_dcache_inval((void *)spif->xip_base, spif->xip_length);
        rt_hw_cpu_icache_ops(RT_HW_CACHE_INVALIDATE, (void *)spif->xip_base, spif->xip_length);
        spif->xip = RT_FALSE;
    }
    rt_mutex_release(&spif->lock);

    return RT_EOK;
}

const void *sun252i_spif_xip_window(void)
{
    return spif ? (const void *)spif->xip_base : RT_NULL;
}

rt_err_t sun252i_spif_get_info(struct sun252i_spif_info *info)
{
    if (!spif)
    {
        return -RT_ERROR;
    }
    info->version = spif->version;
    memcpy(info->jedec_id, spif->jedec, 3);
    info->size = spif->size;
    info->frequency = spif->frequency;
    info->quad = spif->quad;
    info->dtr = spif->dtr_cap;
    info->addr_4byte = spif->addr_4byte;
    info->sample_tuned = spif->tuned;
    info->sample_mode = spif->mode;
    info->sample_delay = spif->delay;
    info->xip_active = spif->xip;
    info->xip_flash_offset = spif->xip_offset;
    info->xip_length = spif->xip ? spif->xip_length : 0u;

    return RT_EOK;
}

/* ---- mtd_nor ----------------------------------------------------------- */

#ifdef RT_USING_MTD_NOR
static rt_err_t mtd_read_id(struct rt_mtd_nor_device *device)
{
    return (spif->jedec[0] << 16) | (spif->jedec[1] << 8) | spif->jedec[2];
}

static rt_ssize_t mtd_read(struct rt_mtd_nor_device *device, rt_off_t offset, rt_uint8_t *data, rt_size_t length)
{
    return sun252i_spif_read(offset, data, length);
}

static rt_ssize_t mtd_write(struct rt_mtd_nor_device *device, rt_off_t offset, const rt_uint8_t *data, rt_size_t length)
{
    return sun252i_spif_write(offset, data, length);
}

static rt_err_t mtd_erase(struct rt_mtd_nor_device *device, rt_off_t offset, rt_size_t length)
{
    return sun252i_spif_erase(offset, length);
}

static const struct rt_mtd_nor_driver_ops mtd_ops =
{
    mtd_read_id,
    mtd_read,
    mtd_write,
    mtd_erase,
};
#endif

/* ---- msh --------------------------------------------------------------- */

static int spif_info(int argc, char **argv)
{
    struct sun252i_spif_info i;

    if (sun252i_spif_get_info(&i) != RT_EOK)
    {
        rt_kprintf("spif: not probed\n");
        return -1;
    }
    rt_kprintf("controller %08x, flash %02x%02x%02x %u KiB, %u Hz, quad %d dtr %d, sample %s mode %u delay %u, xip %d\n",
               i.version, i.jedec_id[0], i.jedec_id[1], i.jedec_id[2], i.size / 1024u, i.frequency,
               i.quad, i.dtr, i.sample_tuned ? "tuned" : "none", i.sample_mode, i.sample_delay,
               i.xip_active);

    return 0;
}
MSH_CMD_EXPORT(spif_info, SPIF controller and flash state);

static int spif_tune_cmd(int argc, char **argv)
{
    struct sun252i_spif_tune_result r;
    int ret = sun252i_spif_tune(&r), m, d;

    rt_kprintf("tune: %d, %u Hz, mode %u delay %u window %u+%u\n", ret, r.frequency, r.mode, r.delay,
               r.window_start, r.window_len);
    for (m = 0; m < SUN252I_SPIF_TUNE_MODES; m++)
    {
        char line[SUN252I_SPIF_TUNE_DELAYS + 1], one[SUN252I_SPIF_TUNE_DELAYS + 1];

        for (d = 0; d < SUN252I_SPIF_TUNE_DELAYS; d++)
        {
            line[d] = (r.ok[m] >> d) & 1u ? '#' : '.';
            one[d] = (r.ok_one_wire[m] >> d) & 1u ? '#' : '.';
        }
        line[SUN252I_SPIF_TUNE_DELAYS] = '\0';
        one[SUN252I_SPIF_TUNE_DELAYS] = '\0';
        rt_kprintf("  mode %d %s  (id only %s)\n", m, line, one);
    }

    return ret;
}
MSH_CMD_EXPORT_ALIAS(spif_tune_cmd, spif_tune, search the SPIF sample point);

/* ---- probe ------------------------------------------------------------- */

static rt_err_t spif_hw_init(void)
{
    rt_uint8_t sr1 = 0, sr2 = 0;
    int ret;

    spif->frequency = spif_set_clock(IDENT_FREQ);
    spif_hw_setup();

    spif->version = spif_rd(SPIF_VER);
    if (spif->version < SPIF_VER_V1)
    {
        rt_kprintf("spif: controller version %08x is not supported\n", spif->version);
        return -RT_ENOSYS;
    }

    ret = nor_simple(NOR_RDID, 3, RT_NULL, 0);
    if (ret != 0)
    {
        return ret;
    }
    memcpy(spif->jedec, spif->dma->buf, 3);
    if ((spif->jedec[0] == 0xff && spif->jedec[1] == 0xff && spif->jedec[2] == 0xff) ||
        (spif->jedec[0] == 0 && spif->jedec[1] == 0 && spif->jedec[2] == 0))
    {
        rt_kprintf("spif: no flash answers (JEDEC ID %02x%02x%02x)\n", spif->jedec[0], spif->jedec[1], spif->jedec[2]);
        return -RT_EIO;
    }
    if (spif->has_jedec && memcmp(spif->jedec_cfg, spif->jedec, 3) != 0)
    {
        rt_kprintf("spif: JEDEC ID %02x%02x%02x, expected %02x%02x%02x\n", spif->jedec[0], spif->jedec[1],
                   spif->jedec[2], spif->jedec_cfg[0], spif->jedec_cfg[1], spif->jedec_cfg[2]);
        return -RT_EIO;
    }

    spif->size = spif->size_cfg != 0u ? spif->size_cfg : (1u << spif->jedec[2]);
    spif->addr_4byte = spif->addr_4byte_cfg || spif->size > (1u << 24);
    rt_kprintf("spif: flash %02x%02x%02x, %u KiB, controller %08x\n", spif->jedec[0], spif->jedec[1],
               spif->jedec[2], spif->size / 1024u, spif->version);

    if (spif->quad_cfg)
    {
        if (nor_enable_quad() == 0)
        {
            spif->quad = RT_TRUE;
        }
        else
        {
            rt_kprintf("spif: quad enable failed, using one wire\n");
        }
    }

    if (nor_read_status(NOR_RDSR, &sr1) == 0)
    {
        (void)nor_read_status(NOR_RDSR2, &sr2);
    }
    rt_kprintf("spif: status registers %02x %02x\n", sr1, sr2);
    if (spif->clear_bp)
    {
        ret = nor_clear_protect(&sr1);
        if (ret != 0)
        {
            rt_kprintf("spif: could not clear the block protect bits: %d\n", ret);
        }
        else
        {
            (void)nor_read_status(NOR_RDSR2, &sr2);
            rt_kprintf("spif: status registers now %02x %02x\n", sr1, sr2);
        }
    }

    spif->tuned = RT_FALSE;
    spif_dtr_probe();
    if (spif->frequency_cfg > IDENT_FREQ)
    {
        ret = spif->has_params ? sun252i_spif_params_load() : -RT_ENOSYS;
        if (ret == 0)
        {
            rt_kprintf("spif: sample point from the params sector: mode %u delay %u, %u Hz\n", spif->mode,
                       spif->delay, spif->frequency);
        }
        else
        {
            struct sun252i_spif_tune_result res;

            ret = sun252i_spif_tune(&res);
            if (ret == 0)
            {
                rt_kprintf("spif: tuned: mode %u delay %u (window %u..%u), %u Hz\n", res.mode, res.delay,
                           res.window_start, res.window_start + res.window_len - 1u, spif->frequency);
                if (spif->has_params && sun252i_spif_params_save() != 0)
                {
                    rt_kprintf("spif: could not save the sample point\n");
                }
            }
            else
            {
                rt_kprintf("spif: tuning failed (%d), running at %u Hz\n", ret, spif->frequency);
            }
        }
    }

    if (spif->xip_at_init && spif->xip_length_cfg != 0u)
    {
        ret = sun252i_spif_xip_enable(spif->xip_offset_cfg, spif->xip_length_cfg);
        if (ret != 0)
        {
            rt_kprintf("spif: xip enable failed: %d\n", ret);
        }
    }

    return RT_EOK;
}

static rt_err_t spif_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct rt_ofw_node *np = dev->ofw_node;
    rt_uint64_t addr, size;
    rt_uint32_t v;
    rt_uint8_t id[3];
    rt_err_t err;
    struct rt_reset_control *rst;

    if (spif)
    {
        return -RT_EBUSY;
    }
    spif = rt_calloc(1, sizeof(*spif));
    if (!spif)
    {
        return -RT_ENOMEM;
    }
    spif->dma = rt_malloc_align(sizeof(*spif->dma), 64);
    if (!spif->dma)
    {
        err = -RT_ENOMEM;
        goto _fail;
    }
    memset(spif->dma, 0, sizeof(*spif->dma));

    spif->base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    if (!spif->base || rt_dm_dev_get_address(dev, 1, &addr, &size) != RT_EOK)
    {
        err = -RT_EIO;
        goto _fail;
    }
    spif->xip_base = (rt_ubase_t)addr;
    spif->xip_size = (rt_size_t)size;

    spif->bus = rt_clk_get_by_name(dev, "bus");
    spif->mod = rt_clk_get_by_name(dev, "mod");
    rst = rt_reset_control_get_by_index(dev, 0);
    if (rt_is_err_or_null(spif->bus) || rt_is_err_or_null(spif->mod) || rt_is_err_or_null(rst))
    {
        err = -RT_ERROR;
        goto _fail;
    }

    rt_ofw_prop_read_u32(np, "clock-frequency", &spif->frequency_cfg);
    if (spif->frequency_cfg == 0u)
    {
        spif->frequency_cfg = 100000000u;
    }
    if (!rt_ofw_prop_read_u32(np, "size", &v))
    {
        spif->size_cfg = v / 8u;
    }
    if (rt_ofw_prop_read_u8_array_index(np, "jedec-id", 0, 3, id) == 3)
    {
        spif->has_jedec = RT_TRUE;
        memcpy(spif->jedec_cfg, id, 3);
    }
    spif->quad_cfg = rt_ofw_prop_read_bool(np, "quad");
    spif->dtr_cfg = rt_ofw_prop_read_bool(np, "dtr");
    spif->dtr_opcode_cfg = 0xee;
    if (!rt_ofw_prop_read_u32(np, "dtr-read-opcode", &v))
    {
        spif->dtr_opcode_cfg = v;
    }
    if (!rt_ofw_prop_read_u32(np, "dtr-read-dummy", &v))
    {
        spif->dtr_dummy_cfg = v;
    }
    spif->addr_4byte_cfg = rt_ofw_prop_read_bool(np, "address-4byte");
    spif->clear_bp = rt_ofw_prop_read_bool(np, "clear-block-protect");
    if (!rt_ofw_prop_read_u32(np, "params-offset", &v))
    {
        spif->has_params = RT_TRUE;
        spif->params_offset = v;
    }
    if (!rt_ofw_prop_read_u32(np, "tune-offset", &v))
    {
        spif->tune_offset = v;
    }
    if (!rt_ofw_prop_read_u32(np, "xip-offset", &v))
    {
        spif->xip_offset_cfg = v;
        if (!rt_ofw_prop_read_u32(np, "xip-length", &v))
        {
            spif->xip_length_cfg = v;
            spif->xip_at_init = RT_TRUE;
        }
    }

    rt_mutex_init(&spif->lock, "spif", RT_IPC_FLAG_PRIO);
    spif->dirty_lo = 0xffffffffu;

    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(spif->bus);
    rt_clk_prepare_enable(spif->mod);
    rt_reset_control_reset(rst);

    if ((err = spif_hw_init()) != RT_EOK)
    {
        goto _fail;
    }

#ifdef RT_USING_MTD_NOR
    spif->mtd.block_size = SPIF_SECTOR_SIZE;
    spif->mtd.block_start = 0;
    spif->mtd.block_end = spif->size / SPIF_SECTOR_SIZE;
    spif->mtd.ops = &mtd_ops;
    rt_mtd_nor_register_device("spif0", &spif->mtd);
#endif

    return RT_EOK;

_fail:
    if (spif->dma)
    {
        rt_free_align(spif->dma);
    }
    rt_free(spif);
    spif = RT_NULL;

    return err;
}

static const struct rt_ofw_node_id spif_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-spif" },
    { /* sentinel */ }
};

static struct rt_platform_driver spif_driver =
{
    .name = "spif-sun252i",
    .ids = spif_ofw_ids,
    .probe = spif_probe,
};
RT_PLATFORM_DRIVER_EXPORT(spif_driver);
