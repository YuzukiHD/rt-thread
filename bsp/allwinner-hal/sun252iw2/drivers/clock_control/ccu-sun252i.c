/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock control unit of the sun252i SoC: the clock and the reset provider of the
 * device tree. A clock or reset specifier is an id of dt-bindings/clock/allwinner-ccu.h
 * (register offset and bit), so every gate and reset line is addressed without a table:
 *
 *  - a bit below 31 of a bus gating register is a plain gate (a reset line is the bit 16 above it),
 *  - bit 31 of a module clock register is the gate of a clock with a source mux and dividers,
 *    which are described by the module table below,
 *  - the PLL_PERI outputs are rate only clocks.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>

#define DBG_TAG "ccu"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#include <dt-bindings/clock/sun252iw2-ccu.h>

#define HOSC_HZ         24000000u
#define PLL_PERI_REG    0x0020u

#define MOD_GATE        RT_BIT(31)
#define MOD_MUX_SHIFT   24
#define MOD_MUX_MASK    (7u << MOD_MUX_SHIFT)
#define MOD_N_SHIFT     8

enum ccu_src
{
    SRC_NONE = 0,
    SRC_HOSC,
    SRC_PERI_2X,
    SRC_PERI_1X,
    SRC_PERI_480M,
    SRC_VIDEO0_4X,
    SRC_VIDEO0_1X,
};

/* PLL_VIDEO0: 24 MHz * N (optionally halved at the input), the pixel clock PLL */
#define PLL_VIDEO0_REG      0x0040u
#define PLL_VIDEO0_DIV2     RT_BIT(1)
#define PLL_VIDEO0_N_SHIFT  8
#define PLL_VIDEO0_N_MASK   (0xffu << PLL_VIDEO0_N_SHIFT)
#define PLL_VIDEO0_LDO      RT_BIT(30)
#define PLL_VIDEO0_EN       RT_BIT(31)
#define PLL_VIDEO0_OUT      RT_BIT(27)
#define PLL_VIDEO0_LOCK_EN  RT_BIT(29)
#define PLL_VIDEO0_LOCKED   RT_BIT(28)
#define PLL_VIDEO0_MIN_HZ   288000000u
#define PLL_VIDEO0_MAX_HZ   2400000000u

/* A module clock: gate at bit 31, source mux at 26:24, power of two divider N at bit 8 and
 * a linear divider M - 1 at bit 0; the rate is source / (M * 2^N). Only the sources
 * whose mux value is known are listed. */
struct ccu_mod
{
    rt_uint16_t reg;
    rt_uint8_t n_bits;
    rt_uint8_t m_bits;
    rt_bool_t owns_video_pll;   /* the rate request retunes PLL_VIDEO0 (pixel clock) */
    enum ccu_src src[5];
};

static const struct ccu_mod ccu_mods[] =
{
    { 0x0600, 0, 5, RT_FALSE, { SRC_PERI_2X, SRC_VIDEO0_4X } },     /* display engine */
    { 0x0630, 0, 5, RT_FALSE, { SRC_PERI_2X } },                    /* G2D */
    { 0x0690, 0, 5, RT_FALSE, { SRC_PERI_480M, SRC_PERI_2X } },     /* video engine */
    { 0x0790, 0, 5, RT_FALSE, { SRC_PERI_2X } },                    /* panel backlight */
    { 0x0830, 2, 4, RT_FALSE, { SRC_HOSC, SRC_PERI_1X } },          /* SMHC0 */
    { 0x0940, 4, 0, RT_FALSE, { SRC_HOSC } },                       /* SPI0 */
    { 0x0944, 4, 0, RT_FALSE, { SRC_HOSC } },                       /* SPI1 (and the display bus interface) */
    { 0x0aa0, 0, 5, RT_FALSE, { SRC_VIDEO0_4X, SRC_PERI_2X } },     /* combo D-PHY */
    { 0x0b24, 0, 4, RT_FALSE, { SRC_HOSC, SRC_PERI_1X } },          /* MIPI DSI */
    { 0x0b60, 2, 4, RT_TRUE,  { SRC_VIDEO0_1X, SRC_VIDEO0_4X, SRC_PERI_2X } }, /* TCON LCD */
};


/* ---- audio module clocks and their PLL ---------------------------------------------------
 * PLL_AUDIO1 runs in one of two families: 3.072 GHz (/5 = 614.4 MHz, multiples of 48 kHz) or
 * 2.1676 GHz with the fractional pattern (/2 = 1083.8 MHz, 44.1 kHz family). The module clocks
 * select one of its two outputs; the PLL is on while one of them runs and cannot serve both families.
 */
#define PLL_AUDIO1_REG      0x0080u
#define PLL_PAT0_REG        0x0180u
#define PLL_PAT1_REG        0x0184u
#define PLL_SDM_EN          RT_BIT(24)
#define PLL_OUT             RT_BIT(27)
#define PLL_LOCKED          RT_BIT(28)
#define PLL_LOCK_EN         RT_BIT(29)
#define PLL_LDO             RT_BIT(30)
#define PLL_EN              RT_BIT(31)
#define PLL_N_SHIFT         8
#define PLL_DIV2_SHIFT      16
#define PLL_DIV5_SHIFT      20
#define PLL_DIV_MASK        0x7u
#define PLL_PAT0_44K1       0xc000a234u
#define PLL_48K_N           128u
#define PLL_48K_DIV         5u
#define PLL_44K1_N          90u
#define PLL_44K1_RATE       2167603200u
#define PLL_44K1_DIV        2u
#define FAMILY_48K          0
#define FAMILY_44K1         1

struct ccu_audio_mod
{
    rt_uint16_t reg;
    rt_uint8_t mux_div2;    /* mux value that selects the /2 output */
    rt_uint8_t mux_div5;    /* and the /5 output */
};

static const struct ccu_audio_mod ccu_audio_mods[] =
{
    { 0x0a10, 0, 1 },   /* I2S0 */
    { 0x0a24, 0, 1 },   /* S/PDIF transmitter */
    { 0x0a28, 1, 2 },   /* S/PDIF receiver */
    { 0x0a50, 0, 1 },   /* codec DAC */
    { 0x0a54, 0, 1 },   /* codec ADC */
};

struct ccu
{
    struct rt_clk_node node;
    struct rt_reset_controller rstc;
    struct rt_clk_cell *cells[1];
    void *base;
    struct rt_spinlock lock;
    rt_list_t clks;
    struct rt_mutex pll_lock;
    rt_int32_t pll_users;
    rt_int32_t pll_family;
};

struct ccu_clk
{
    struct rt_clk_cell cell;
    rt_list_t list;
    rt_uint32_t id;
    rt_uint16_t reg;
    rt_uint8_t bit;
    const struct ccu_mod *mod;
    const struct ccu_audio_mod *amod;
    rt_bool_t holds_pll;
    struct ccu *ccu;
};

#define cell_to_clk(c) rt_container_of(c, struct ccu_clk, cell)

static rt_uint32_t ccu_read(struct ccu *ccu, rt_uint32_t reg)
{
    return HWREG32((rt_ubase_t)ccu->base + reg);
}

static void ccu_update(struct ccu *ccu, rt_uint32_t reg, rt_uint32_t mask, rt_uint32_t val)
{
    rt_ubase_t level = rt_spin_lock_irqsave(&ccu->lock);

    HWREG32((rt_ubase_t)ccu->base + reg) = (ccu_read(ccu, reg) & ~mask) | (val & mask);
    rt_spin_unlock_irqrestore(&ccu->lock, level);
}

static rt_uint32_t video_pll_rate(struct ccu *ccu)
{
    rt_uint32_t reg = ccu_read(ccu, PLL_VIDEO0_REG);

    if (!(reg & PLL_VIDEO0_EN))
    {
        return 0;
    }

    return HOSC_HZ * (((reg & PLL_VIDEO0_N_MASK) >> PLL_VIDEO0_N_SHIFT) + 1u) /
           ((reg & PLL_VIDEO0_DIV2) ? 2u : 1u);
}

static rt_uint32_t ccu_src_rate(struct ccu *ccu, enum ccu_src src)
{
    rt_uint32_t pll = ccu_read(ccu, PLL_PERI_REG);
    rt_uint32_t n = ((pll >> 8) & 0xffu) + 1u;
    rt_uint32_t p0 = ((pll >> 16) & 0x7u) + 1u;
    rt_uint32_t m = (pll & RT_BIT(1)) ? 2u : 1u;
    rt_uint32_t raw = HOSC_HZ / m / p0 * n;

    switch (src)
    {
    case SRC_VIDEO0_4X: return video_pll_rate(ccu);
    case SRC_VIDEO0_1X: return video_pll_rate(ccu) / 4u;
    case SRC_HOSC:      return HOSC_HZ;
    case SRC_PERI_2X:   return raw;
    case SRC_PERI_1X:   return raw / 2u;
    case SRC_PERI_480M: return raw / (((pll >> 2) & 0x7u) + 1u);
    default:            return 0;
    }
}

/* ---- gates ------------------------------------------------------------------ */
static rt_err_t ccu_gate_enable(struct rt_clk_cell *cell)
{
    struct ccu_clk *c = cell_to_clk(cell);

    ccu_update(c->ccu, c->reg, RT_BIT(c->bit), RT_BIT(c->bit));

    return RT_EOK;
}

static void ccu_gate_disable(struct rt_clk_cell *cell)
{
    struct ccu_clk *c = cell_to_clk(cell);

    ccu_update(c->ccu, c->reg, RT_BIT(c->bit), 0);
}

static rt_bool_t ccu_gate_is_enabled(struct rt_clk_cell *cell)
{
    struct ccu_clk *c = cell_to_clk(cell);

    return !!(ccu_read(c->ccu, c->reg) & RT_BIT(c->bit));
}

static const struct rt_clk_ops ccu_gate_ops =
{
    .enable = ccu_gate_enable,
    .disable = ccu_gate_disable,
    .is_enabled = ccu_gate_is_enabled,
};

/* ---- PLL_PERI outputs: rate only ------------------------------------------------ */
static rt_ubase_t ccu_pll_recalc_rate(struct rt_clk_cell *cell, rt_ubase_t parent_rate)
{
    struct ccu_clk *c = cell_to_clk(cell);

    return ccu_src_rate(c->ccu, c->bit == 16 ? SRC_PERI_2X : c->bit == 17 ? SRC_PERI_1X : SRC_PERI_480M);
}

static const struct rt_clk_ops ccu_pll_ops =
{
    .recalc_rate = ccu_pll_recalc_rate,
};

/* ---- PLL_VIDEO0 ------------------------------------------------------------------- */
static rt_err_t video_pll_start(struct ccu *ccu)
{
    rt_uint32_t reg = ccu_read(ccu, PLL_VIDEO0_REG);
    rt_int32_t tries = 10000;

    if ((reg & PLL_VIDEO0_EN) && (reg & PLL_VIDEO0_OUT))
    {
        return RT_EOK;
    }
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_LDO, PLL_VIDEO0_LDO);
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_EN, PLL_VIDEO0_EN);
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_LOCK_EN, PLL_VIDEO0_LOCK_EN);
    while (tries-- > 0 && !(ccu_read(ccu, PLL_VIDEO0_REG) & PLL_VIDEO0_LOCKED))
    {
        rt_hw_us_delay(10);
    }
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_OUT, PLL_VIDEO0_OUT);

    return RT_EOK;
}

static void video_pll_stop(struct ccu *ccu)
{
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_OUT, 0);
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_EN, 0);
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_LDO, 0);
}

/* the multiplier N that brings 24 MHz * N closest to the wanted rate */
static rt_uint32_t video_pll_pick(rt_ubase_t hz)
{
    rt_uint32_t n;

    hz = hz < PLL_VIDEO0_MIN_HZ ? PLL_VIDEO0_MIN_HZ : hz > PLL_VIDEO0_MAX_HZ ? PLL_VIDEO0_MAX_HZ : hz;
    n = (hz + HOSC_HZ / 2u) / HOSC_HZ;

    return n < 1u ? 1u : n > 256u ? 256u : n;
}

static rt_err_t video_pll_set(struct ccu *ccu, rt_ubase_t hz)
{
    rt_uint32_t n = video_pll_pick(hz);
    rt_bool_t was_on = !!(ccu_read(ccu, PLL_VIDEO0_REG) & PLL_VIDEO0_EN);

    if (was_on)
    {
        video_pll_stop(ccu);
    }
    /* the input divider has to be cleared or the output (and the pixel clock) is halved */
    ccu_update(ccu, PLL_VIDEO0_REG, PLL_VIDEO0_DIV2 | PLL_VIDEO0_N_MASK, (n - 1u) << PLL_VIDEO0_N_SHIFT);
    if (was_on)
    {
        video_pll_start(ccu);
    }

    return RT_EOK;
}

static rt_err_t ccu_vpll_enable(struct rt_clk_cell *cell)
{
    return video_pll_start(cell_to_clk(cell)->ccu);
}

static void ccu_vpll_disable(struct rt_clk_cell *cell)
{
    /* shared with the other display clocks: it stays running */
}

static rt_bool_t ccu_vpll_is_enabled(struct rt_clk_cell *cell)
{
    return !!(ccu_read(cell_to_clk(cell)->ccu, PLL_VIDEO0_REG) & PLL_VIDEO0_EN);
}

static rt_ubase_t ccu_vpll_recalc_rate(struct rt_clk_cell *cell, rt_ubase_t parent_rate)
{
    return video_pll_rate(cell_to_clk(cell)->ccu);
}

static rt_base_t ccu_vpll_round_rate(struct rt_clk_cell *cell, rt_ubase_t drate, rt_ubase_t *prate)
{
    return (rt_base_t)(video_pll_pick(drate) * HOSC_HZ);
}

static rt_err_t ccu_vpll_set_rate(struct rt_clk_cell *cell, rt_ubase_t hz, rt_ubase_t parent_rate)
{
    return video_pll_set(cell_to_clk(cell)->ccu, hz);
}

static const struct rt_clk_ops ccu_vpll_ops =
{
    .enable = ccu_vpll_enable,
    .disable = ccu_vpll_disable,
    .is_enabled = ccu_vpll_is_enabled,
    .recalc_rate = ccu_vpll_recalc_rate,
    .round_rate = ccu_vpll_round_rate,
    .set_rate = ccu_vpll_set_rate,
};

/* ---- module clocks ------------------------------------------------------------- */
static rt_bool_t mod_best_div(const struct ccu_mod *m, rt_uint32_t src_hz, rt_uint32_t hz,
                              rt_uint32_t *n_out, rt_uint32_t *m_out, rt_uint32_t *rate_out)
{
    rt_uint32_t n, d, best_err = 0xffffffffu;
    rt_bool_t found = RT_FALSE;

    for (n = 0; n < (1u << m->n_bits); n++)
    {
        for (d = 1; d <= (1u << m->m_bits); d++)
        {
            rt_uint32_t rate = (src_hz >> n) / d, err;

            if (!rate)
            {
                continue;
            }
            err = rate > hz ? rate - hz : hz - rate;
            if (err < best_err)
            {
                best_err = err;
                *n_out = n;
                *m_out = d;
                *rate_out = rate;
                found = RT_TRUE;
            }
        }
    }

    return found;
}

static rt_ubase_t ccu_mod_recalc_rate(struct rt_clk_cell *cell, rt_ubase_t parent_rate)
{
    struct ccu_clk *c = cell_to_clk(cell);
    rt_uint32_t v = ccu_read(c->ccu, c->reg);
    rt_uint32_t mux = (v & MOD_MUX_MASK) >> MOD_MUX_SHIFT;
    rt_uint32_t n = (v >> MOD_N_SHIFT) & ((1u << c->mod->n_bits) - 1u);
    rt_uint32_t d = (v & ((1u << c->mod->m_bits) - 1u)) + 1u;

    if (mux >= RT_ARRAY_SIZE(c->mod->src) || c->mod->src[mux] == SRC_NONE)
    {
        return 0;
    }

    return (ccu_src_rate(c->ccu, c->mod->src[mux]) >> n) / d;
}

/* the source and the dividers that come closest to the wanted rate */
static rt_bool_t ccu_mod_pick(struct ccu_clk *c, rt_ubase_t hz, rt_uint32_t *mux, rt_uint32_t *n,
                              rt_uint32_t *d, rt_uint32_t *rate)
{
    rt_uint32_t i, best_err = 0xffffffffu;
    rt_bool_t found = RT_FALSE;

    for (i = 0; i < RT_ARRAY_SIZE(c->mod->src); i++)
    {
        rt_uint32_t src_hz, tn, td, tr, err;

        if (c->mod->src[i] == SRC_NONE)
        {
            continue;
        }
        src_hz = ccu_src_rate(c->ccu, c->mod->src[i]);
        if (!src_hz || !mod_best_div(c->mod, src_hz, hz, &tn, &td, &tr))
        {
            continue;
        }
        err = tr > hz ? tr - hz : hz - tr;
        if (err < best_err)
        {
            best_err = err;
            *mux = i;
            *n = tn;
            *d = td;
            *rate = tr;
            found = RT_TRUE;
        }
    }

    return found;
}

static rt_base_t ccu_mod_round_rate(struct rt_clk_cell *cell, rt_ubase_t drate, rt_ubase_t *prate)
{
    rt_uint32_t mux, n, d, rate;

    if (cell_to_clk(cell)->mod->owns_video_pll)
    {
        return (rt_base_t)(video_pll_pick(drate) * HOSC_HZ);
    }

    if (!ccu_mod_pick(cell_to_clk(cell), drate, &mux, &n, &d, &rate))
    {
        return -RT_EINVAL;
    }

    return rate;
}

static rt_err_t ccu_mod_set_rate(struct rt_clk_cell *cell, rt_ubase_t hz, rt_ubase_t parent_rate)
{
    struct ccu_clk *c = cell_to_clk(cell);
    rt_uint32_t mux, n, d, rate, mask, val;

    if (c->mod->owns_video_pll)
    {
        /* the pixel clock PLL is dedicated to this clock: retune it, no divider */
        video_pll_set(c->ccu, hz);
        mux = 1;    /* PLL_VIDEO0_4X */
        n = 0;
        d = 1;
    }
    else if (!ccu_mod_pick(c, hz, &mux, &n, &d, &rate))
    {
        return -RT_EINVAL;
    }

    mask = MOD_MUX_MASK | (((1u << c->mod->n_bits) - 1u) << MOD_N_SHIFT) | ((1u << c->mod->m_bits) - 1u);
    val = (mux << MOD_MUX_SHIFT) | (n << MOD_N_SHIFT) | (d - 1u);

    /* the dividers change with the gate closed */
    if (ccu_read(c->ccu, c->reg) & MOD_GATE)
    {
        ccu_update(c->ccu, c->reg, MOD_GATE, 0);
        ccu_update(c->ccu, c->reg, mask, val);
        ccu_update(c->ccu, c->reg, MOD_GATE, MOD_GATE);
    }
    else
    {
        ccu_update(c->ccu, c->reg, mask, val);
    }

    return RT_EOK;
}

static rt_err_t ccu_mod_enable(struct rt_clk_cell *cell)
{
    struct ccu_clk *c = cell_to_clk(cell);
    rt_uint32_t mux = (ccu_read(c->ccu, c->reg) & MOD_MUX_MASK) >> MOD_MUX_SHIFT;

    /* the selected PLL has to run before the gate opens */
    if (mux < RT_ARRAY_SIZE(c->mod->src) &&
        (c->mod->src[mux] == SRC_VIDEO0_4X || c->mod->src[mux] == SRC_VIDEO0_1X))
    {
        video_pll_start(c->ccu);
    }

    return ccu_gate_enable(cell);
}

static const struct rt_clk_ops ccu_mod_ops =
{
    .enable = ccu_mod_enable,
    .disable = ccu_gate_disable,
    .is_enabled = ccu_gate_is_enabled,
    .recalc_rate = ccu_mod_recalc_rate,
    .round_rate = ccu_mod_round_rate,
    .set_rate = ccu_mod_set_rate,
};


static rt_uint32_t audio_pll_rate(rt_int32_t family)
{
    return family == FAMILY_48K ? HOSC_HZ * PLL_48K_N / PLL_48K_DIV : PLL_44K1_RATE / PLL_44K1_DIV;
}

static rt_err_t audio_pll_start(struct ccu *ccu, rt_int32_t family)
{
    rt_uint32_t reg = ccu_read(ccu, PLL_AUDIO1_REG);
    rt_uint32_t n = family == FAMILY_48K ? PLL_48K_N : PLL_44K1_N;
    rt_int32_t tries = 2000;

    reg &= ~(PLL_OUT | PLL_EN | PLL_LDO | PLL_LOCK_EN | PLL_SDM_EN | (0xffu << PLL_N_SHIFT) |
             (PLL_DIV_MASK << PLL_DIV2_SHIFT) | (PLL_DIV_MASK << PLL_DIV5_SHIFT));
    HWREG32((rt_ubase_t)ccu->base + PLL_AUDIO1_REG) = reg;
    reg |= ((n - 1u) << PLL_N_SHIFT) | ((PLL_44K1_DIV - 1u) << PLL_DIV2_SHIFT) | ((PLL_48K_DIV - 1u) << PLL_DIV5_SHIFT);
    if (family == FAMILY_44K1)
    {
        HWREG32((rt_ubase_t)ccu->base + PLL_PAT0_REG) = PLL_PAT0_44K1;
        HWREG32((rt_ubase_t)ccu->base + PLL_PAT1_REG) = 0;
        reg |= PLL_SDM_EN;
    }
    else
    {
        HWREG32((rt_ubase_t)ccu->base + PLL_PAT0_REG) = 0;
        HWREG32((rt_ubase_t)ccu->base + PLL_PAT1_REG) = 0;
    }
    HWREG32((rt_ubase_t)ccu->base + PLL_AUDIO1_REG) = reg;
    HWREG32((rt_ubase_t)ccu->base + PLL_AUDIO1_REG) = reg | PLL_LDO;
    HWREG32((rt_ubase_t)ccu->base + PLL_AUDIO1_REG) = reg | PLL_LDO | PLL_EN;
    HWREG32((rt_ubase_t)ccu->base + PLL_AUDIO1_REG) = reg | PLL_LDO | PLL_EN | PLL_LOCK_EN;
    while (tries-- > 0 && !(ccu_read(ccu, PLL_AUDIO1_REG) & PLL_LOCKED))
    {
        rt_hw_us_delay(1);
    }
    if (!(ccu_read(ccu, PLL_AUDIO1_REG) & PLL_LOCKED))
    {
        HWREG32((rt_ubase_t)ccu->base + PLL_AUDIO1_REG) = reg & ~(PLL_EN | PLL_LDO | PLL_LOCK_EN);
        return -RT_ETIMEOUT;
    }
    ccu_update(ccu, PLL_AUDIO1_REG, PLL_OUT, PLL_OUT);

    return RT_EOK;
}

static rt_err_t audio_pll_get(struct ccu *ccu, rt_int32_t family)
{
    rt_err_t err = RT_EOK;

    rt_mutex_take(&ccu->pll_lock, RT_WAITING_FOREVER);
    if (ccu->pll_users == 0)
    {
        err = audio_pll_start(ccu, family);
        if (!err)
        {
            ccu->pll_family = family;
            ccu->pll_users = 1;
        }
    }
    else if (ccu->pll_family != family)
    {
        err = -RT_EBUSY;
    }
    else
    {
        ccu->pll_users++;
    }
    rt_mutex_release(&ccu->pll_lock);

    return err;
}

static void audio_pll_put(struct ccu *ccu)
{
    rt_mutex_take(&ccu->pll_lock, RT_WAITING_FOREVER);
    if (ccu->pll_users > 0 && --ccu->pll_users == 0)
    {
        rt_uint32_t reg = ccu_read(ccu, PLL_AUDIO1_REG);

        ccu_update(ccu, PLL_AUDIO1_REG, PLL_OUT, 0);
        ccu_update(ccu, PLL_AUDIO1_REG, PLL_EN | PLL_LDO | PLL_LOCK_EN | PLL_SDM_EN, 0);
        RT_UNUSED(reg);
    }
    rt_mutex_release(&ccu->pll_lock);
}

static rt_int32_t audio_family(rt_ubase_t rate)
{
    if (rate && rate % 8000u == 0u)
    {
        return FAMILY_48K;
    }
    if (rate && (rate % 11025u == 0u))
    {
        return FAMILY_44K1;
    }

    return -1;
}

static rt_ubase_t ccu_audio_recalc_rate(struct rt_clk_cell *cell, rt_ubase_t parent_rate)
{
    struct ccu_clk *c = cell_to_clk(cell);
    rt_uint32_t v = ccu_read(c->ccu, c->reg);
    rt_uint32_t p = (v >> 8) & 3u, m = (v & 0x1fu) + 1u;

    if (!c->ccu->pll_users)
    {
        return 0;
    }

    return audio_pll_rate(c->ccu->pll_family) / (m << p);
}

/* the family of the rate decides the PLL; the dividers come as close to the rate as they can */
static rt_err_t ccu_audio_set_rate(struct rt_clk_cell *cell, rt_ubase_t rate, rt_ubase_t parent_rate)
{
    struct ccu_clk *c = cell_to_clk(cell);
    rt_int32_t family = audio_family(rate);
    rt_uint32_t src, div, best_p = 0, best_m = 1, best_err = 0xffffffffu, p, m;
    rt_err_t err;

    if (family < 0)
    {
        return -RT_EINVAL;
    }
    if (!c->holds_pll)
    {
        if ((err = audio_pll_get(c->ccu, family)))
        {
            return err;
        }
        c->holds_pll = RT_TRUE;
    }
    else if (c->ccu->pll_family != family)
    {
        return -RT_EBUSY;
    }

    src = audio_pll_rate(family);
    div = (src + rate / 2u) / rate;
    for (p = 0; p < 4u; p++)
    {
        rt_uint32_t got, e;

        m = (div + (1u << p) / 2u) / (1u << p);
        if (m < 1u || m > 32u)
        {
            continue;
        }
        got = src / (m << p);
        e = got > rate ? got - rate : rate - got;
        if (e < best_err)
        {
            best_err = e;
            best_p = p;
            best_m = m;
        }
    }
    if (best_err > rate / 100u)
    {
        return -RT_EINVAL;
    }

    ccu_update(c->ccu, c->reg, MOD_GATE, 0);
    ccu_update(c->ccu, c->reg, 0x7fffffffu,
               ((family == FAMILY_48K ? c->amod->mux_div5 : c->amod->mux_div2) << MOD_MUX_SHIFT) |
               (best_p << MOD_N_SHIFT) | (best_m - 1u));
    ccu_update(c->ccu, c->reg, MOD_GATE, MOD_GATE);

    return RT_EOK;
}

static rt_base_t ccu_audio_round_rate(struct rt_clk_cell *cell, rt_ubase_t rate, rt_ubase_t *prate)
{
    return audio_family(rate) < 0 ? -RT_EINVAL : (rt_base_t)rate;
}

static void ccu_audio_disable(struct rt_clk_cell *cell)
{
    struct ccu_clk *c = cell_to_clk(cell);

    ccu_update(c->ccu, c->reg, MOD_GATE, 0);
    if (c->holds_pll)
    {
        c->holds_pll = RT_FALSE;
        audio_pll_put(c->ccu);
    }
}

static const struct rt_clk_ops ccu_audio_ops =
{
    .enable = ccu_gate_enable,
    .disable = ccu_audio_disable,
    .is_enabled = ccu_gate_is_enabled,
    .recalc_rate = ccu_audio_recalc_rate,
    .round_rate = ccu_audio_round_rate,
    .set_rate = ccu_audio_set_rate,
};

/* ---- provider ------------------------------------------------------------------- */
static struct rt_clk_cell *ccu_ofw_parse(struct rt_clk_node *np, struct rt_ofw_cell_args *args)
{
    struct ccu *ccu = np->priv;
    struct ccu_clk *c;
    rt_uint32_t id = args->args[0];
    rt_uint32_t i;

    rt_list_for_each_entry(c, &ccu->clks, list)
    {
        if (c->id == id)
        {
            return &c->cell;
        }
    }

    if (!(c = rt_calloc(1, sizeof(*c))))
    {
        return RT_NULL;
    }

    c->id = id;
    c->reg = ALLWINNER_CCU_ID_REG(id);
    c->bit = ALLWINNER_CCU_ID_BIT(id);
    c->ccu = ccu;
    c->cell.clk_np = np;
    c->cell.flags = RT_CLK_F_GET_RATE_NOCACHE;
    c->cell.ops = &ccu_gate_ops;
    c->cell.name = "ccu-clk";

    if (c->reg == PLL_PERI_REG && (c->bit == 16 || c->bit == 17 || c->bit == 18))
    {
        c->cell.ops = &ccu_pll_ops;
    }
    else if (c->reg == PLL_VIDEO0_REG && c->bit == 31)
    {
        c->cell.ops = &ccu_vpll_ops;
    }
    else if (c->bit == 31)
    {
        for (i = 0; i < RT_ARRAY_SIZE(ccu_mods); i++)
        {
            if (ccu_mods[i].reg == c->reg)
            {
                c->mod = &ccu_mods[i];
                c->cell.ops = &ccu_mod_ops;
            }
        }
        for (i = 0; i < RT_ARRAY_SIZE(ccu_audio_mods); i++)
        {
            if (ccu_audio_mods[i].reg == c->reg)
            {
                c->amod = &ccu_audio_mods[i];
                c->cell.ops = &ccu_audio_ops;
            }
        }
    }
    rt_list_insert_after(&ccu->clks, &c->list);

    return &c->cell;
}

/* ---- reset lines ------------------------------------------------------------------ */
static void ccu_reset_set(struct rt_reset_control *rstc, rt_bool_t released)
{
    struct ccu *ccu = rstc->rstcer->priv;
    rt_uint32_t reg = ALLWINNER_CCU_ID_REG(rstc->id), bit = ALLWINNER_CCU_ID_BIT(rstc->id);

    ccu_update(ccu, reg, RT_BIT(bit), released ? RT_BIT(bit) : 0);
}

static rt_err_t ccu_reset_assert(struct rt_reset_control *rstc)
{
    ccu_reset_set(rstc, RT_FALSE);

    return RT_EOK;
}

static rt_err_t ccu_reset_deassert(struct rt_reset_control *rstc)
{
    ccu_reset_set(rstc, RT_TRUE);

    return RT_EOK;
}

static rt_err_t ccu_reset_reset(struct rt_reset_control *rstc)
{
    ccu_reset_set(rstc, RT_FALSE);
    rt_hw_us_delay(10);
    ccu_reset_set(rstc, RT_TRUE);

    return RT_EOK;
}

static int ccu_reset_status(struct rt_reset_control *rstc)
{
    struct ccu *ccu = rstc->rstcer->priv;

    /* 1: the line is held in reset */
    return !(ccu_read(ccu, ALLWINNER_CCU_ID_REG(rstc->id)) & RT_BIT(ALLWINNER_CCU_ID_BIT(rstc->id)));
}

static const struct rt_reset_control_ops ccu_reset_ops =
{
    .reset = ccu_reset_reset,
    .assert = ccu_reset_assert,
    .deassert = ccu_reset_deassert,
    .status = ccu_reset_status,
};

static rt_err_t ccu_probe(struct rt_platform_device *pdev)
{
    rt_err_t err;
    struct rt_device *dev = &pdev->parent;
    struct ccu *ccu = rt_calloc(1, sizeof(*ccu));

    if (!ccu)
    {
        return -RT_ENOMEM;
    }

    ccu->base = rt_dm_dev_iomap(dev, 0);
    if (!ccu->base)
    {
        err = -RT_EIO;
        goto _fail;
    }

    rt_spin_lock_init(&ccu->lock);
    rt_mutex_init(&ccu->pll_lock, "apll", RT_IPC_FLAG_PRIO);
    rt_list_init(&ccu->clks);

    ccu->node.dev = dev;
    ccu->node.priv = ccu;
    ccu->node.ofw_parse = ccu_ofw_parse;
    ccu->node.cells_nr = 1;
    ccu->node.cells = ccu->cells;
    ccu->cells[0] = RT_NULL;

    if ((err = rt_clk_register(&ccu->node)))
    {
        goto _fail;
    }

    ccu->rstc.ofw_node = dev->ofw_node;
    ccu->rstc.ops = &ccu_reset_ops;
    ccu->rstc.priv = ccu;

    if ((err = rt_reset_controller_register(&ccu->rstc)))
    {
        goto _fail;
    }

    return RT_EOK;

_fail:
    rt_free(ccu);

    return err;
}

static const struct rt_ofw_node_id ccu_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-ccu" },
    { /* sentinel */ }
};

static struct rt_platform_driver ccu_driver =
{
    .name = "ccu-sun252i",
    .ids = ccu_ofw_ids,
    .probe = ccu_probe,
};

static int ccu_drv_register(void)
{
    rt_platform_driver_register(&ccu_driver);

    return 0;
}
INIT_SUBSYS_EXPORT(ccu_drv_register);
