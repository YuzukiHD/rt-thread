/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Allwinner G2D driver. submit() validates an operation and builds its
 * register command list into a job taken from a fixed pool; a worker thread
 * feeds the jobs to the hardware one at a time, the task-end interrupt
 * completes them.
 */
#include <drivers/clock_time.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <string.h>
#define DBG_TAG "g2d"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#include "g2d.h"
#include "sunxi/g2d_sunxi_hw.h"
#include "sunxi/g2d_sunxi_regs.h"

#define QUEUE_DEPTH         8u
#define MAX_SIZE            8192U
#define THREAD_ID           0
#define CMDLIST_BYTES       2560U
#define TIMEOUT_MS          100

/* internal cache and bandwidth limiter */
#define CACHE_SIZE          0x40U
#define DDR_LIMIT           0x90U

/* one queued operation with its command list */
struct g2d_job {
    struct g2d_op op;
    struct g2d_cmdlist cl;
    size_t head_len;
    rt_bool_t used;
    uint8_t cmd[CMDLIST_BYTES] __aligned(64);
};

static struct g2d_job jobs[QUEUE_DEPTH] __aligned(64);
static struct rt_messagequeue g2d_queue;
static rt_uint8_t g2d_queue_pool[QUEUE_DEPTH * (sizeof(struct g2d_job *) + sizeof(void *))];
static struct rt_semaphore hw_done;
static volatile uint32_t hw_status;
static volatile rt_uint64_t hw_start_tick, hw_ticks;    /* clock time counter: start of the last job, hardware time of it */
static rt_bool_t g2d_up;
static struct rt_clk *g2d_mod_clk;
static rt_ubase_t g2d_base;

static inline uint32_t g2d_rd(uint32_t off) { return HWREG32(g2d_base + off); }
static inline void g2d_wr(uint32_t off, uint32_t v) { HWREG32(g2d_base + off) = v; }
static void g2d_upd(uint32_t off, uint32_t mask, uint32_t val) { g2d_wr(off, (g2d_rd(off) & ~mask) | (val & mask)); }

/* pulse a reset bit: the hardware is out of reset when the bit is set */
static void g2d_pulse_reset(uint32_t bits)
{
    g2d_upd(G2D_RESET, bits, 0);
    g2d_upd(G2D_RESET, bits, bits);
}

static void g2d_hw_open(void)
{
    g2d_upd(G2D_CLK_GATE, G2D_CLK_GATE_CORE, G2D_CLK_GATE_CORE);
    g2d_upd(G2D_MBUS_GATE, G2D_MBUS_GATE_CLK | G2D_MBUS_GATE_RESET, G2D_MBUS_GATE_CLK | G2D_MBUS_GATE_RESET);
    g2d_upd(G2D_RESET, G2D_RESET_CORE, G2D_RESET_CORE);

    g2d_upd(G2D_CLK_GATE, G2D_CLK_GATE_THREAD(THREAD_ID), G2D_CLK_GATE_THREAD(THREAD_ID));
    g2d_upd(G2D_RESET, G2D_RESET_THREAD(THREAD_ID), G2D_RESET_THREAD(THREAD_ID));

    g2d_upd(G2D_CACHE_CTRL, G2D_CACHE_CTRL_EN, G2D_CACHE_CTRL_EN);
    g2d_upd(G2D_CACHE_SIZE0, G2D_CACHE_SIZE_MASK, CACHE_SIZE);
    g2d_upd(G2D_CACHE_SIZE1, G2D_CACHE_SIZE_MASK, CACHE_SIZE);

    /* keep the G2D from starving the display engine and the CPU of memory bandwidth */
    g2d_upd(G2D_DDR_LIMIT, G2D_DDR_LIMIT_EN | G2D_DDR_LIMIT_MASK, G2D_DDR_LIMIT_EN | DDR_LIMIT);
}

static void g2d_hw_recover(void)
{
    g2d_pulse_reset(G2D_RESET_CORE);
    g2d_pulse_reset(G2D_RESET_THREAD(THREAD_ID));
}

static void g2d_hw_start(const struct g2d_job *job)
{
    const uint32_t thread = G2D_THREAD(THREAD_ID);

    g2d_upd(G2D_MODE, G2D_MODE_MASTER, G2D_MODE_MASTER);
    g2d_wr(thread + G2D_THREAD_IRQ_EN, G2D_THREAD_IRQ_TASK_END | G2D_THREAD_IRQ_TIMEOUT);
    g2d_wr(thread + G2D_THREAD_HEAD_LOW, (uint32_t)(uintptr_t)job->cmd);
    g2d_wr(thread + G2D_THREAD_HEAD_HIGH_LEN, FIELD_PREP(G2D_THREAD_HEAD_LEN_MASK, job->head_len));
    g2d_wr(thread + G2D_THREAD_ATTR, FIELD_PREP(G2D_THREAD_ATTR_CMD_NUM_MASK, 0) | G2D_THREAD_ATTR_END_IRQ);
    hw_start_tick = rt_clock_time_get_counter();
    g2d_wr(thread + G2D_THREAD_UPDATE, 1);
}

static void g2d_dump(void)
{
    uint32_t off;

    for (off = 0; off < 0x60; off += 0x10)
        LOG_E("%03x: %08x %08x %08x %08x", off, g2d_rd(off), g2d_rd(off + 4), g2d_rd(off + 8), g2d_rd(off + 12));
    LOG_E("thread: %08x %08x", g2d_rd(G2D_THREAD(THREAD_ID) + G2D_THREAD_IRQ_EN),
            g2d_rd(G2D_THREAD(THREAD_ID) + G2D_THREAD_IRQ_STATUS));
}

static void g2d_isr(int vector, void *param)
{
    const uint32_t status_reg = G2D_THREAD(THREAD_ID) + G2D_THREAD_IRQ_STATUS;
    uint32_t status = g2d_rd(status_reg);

    (void)vector; (void)param;
    hw_ticks = rt_clock_time_get_counter() - hw_start_tick;
    g2d_wr(status_reg, status);     /* write 1 to clear */
    if (status & (G2D_THREAD_IRQ_TASK_END | G2D_THREAD_IRQ_TIMEOUT)) {
        g2d_pulse_reset(G2D_RESET_THREAD(THREAD_ID));
        hw_status = status;
        rt_sem_release(&hw_done);
    }
}

/* ---- cache maintenance ---------------------------------------------------- */

enum cache_op { CACHE_CLEAN, CACHE_CLEAN_INVALIDATE, CACHE_INVALIDATE };

static void cache_rect(const struct g2d_surface *s, const struct g2d_rect *rect, enum cache_op op)
{
    const struct g2d_fmt_info *fmt = g2d_fmt_get(s->format);
    unsigned int p;

    for (p = 0; fmt != NULL && p < fmt->planes; p++) {
        void *start;
        size_t len;

        if (g2d_surface_rect_span(s, rect, p, &start, &len))
            continue;
        switch (op) {
        case CACHE_CLEAN: sunxi_dcache_flush(start, len); break;
        case CACHE_CLEAN_INVALIDATE: sunxi_dcache_flush_inval(start, len); break;
        default: sunxi_dcache_inval(start, len); break;
        }
    }
}

static void job_cache_before(const struct g2d_op *op)
{
    if (op->flags & G2D_FLAG_NO_CACHE_OPS)
        return;
    /* dirty lines of the destination must not be written back over the result */
    cache_rect(&op->dst, &op->dst_rect, CACHE_CLEAN_INVALIDATE);
    if (op->type != G2D_OP_FILL)
        cache_rect(&op->src, &op->src_rect, CACHE_CLEAN);
    if (op->type == G2D_OP_BLEND)
        cache_rect(&op->bg, &op->bg_rect, CACHE_CLEAN);
}

static void job_cache_after(const struct g2d_op *op)
{
    if (!(op->flags & G2D_FLAG_NO_CACHE_OPS))
        cache_rect(&op->dst, &op->dst_rect, CACHE_INVALIDATE);
}

/* ---- worker ---------------------------------------------------------------- */

static int g2d_run_job(struct g2d_job *job)
{
    int ret = 0;

    job_cache_before(&job->op);
    while (rt_sem_trytake(&hw_done) == RT_EOK)
        ;
    g2d_hw_start(job);
    if (rt_sem_take(&hw_done, rt_tick_from_millisecond(TIMEOUT_MS)) != RT_EOK) {
        LOG_E("operation %d timed out", job->op.type);
        g2d_dump();
        g2d_hw_recover();
        ret = -ETIMEDOUT;
    } else if (hw_status & G2D_THREAD_IRQ_TIMEOUT) {
        LOG_E("hardware timeout in operation %d", job->op.type);
        g2d_hw_recover();
        ret = -EIO;
    }
    job_cache_after(&job->op);
    return ret;
}

static void g2d_thread(void *p)
{
    struct g2d_job *job;

    (void)p;
    for (;;) {
        if (rt_mq_recv(&g2d_queue, &job, sizeof(job), RT_WAITING_FOREVER) <= 0)
            continue;
        int status = g2d_run_job(job);

        if (job->op.callback != NULL)
            job->op.callback(&job->op, status, job->op.user_data);
        job->used = RT_FALSE;
    }
}

/* ---- API --------------------------------------------------------------------- */

int g2d_submit(const struct g2d_op *op)
{
    struct g2d_job *job = NULL;
    rt_base_t level;
    unsigned int i;
    int ret;

    level = rt_hw_interrupt_disable();
    for (i = 0; i < QUEUE_DEPTH; i++) {
        if (!jobs[i].used) {
            jobs[i].used = RT_TRUE;
            job = &jobs[i];
            break;
        }
    }
    rt_hw_interrupt_enable(level);
    if (job == NULL)
        return -ENOMEM;

    job->op = *op;
    g2d_cmdlist_init(&job->cl, job->cmd, sizeof(job->cmd));
    ret = g2d_compose(&job->cl, &job->op);
    if (ret) {
        job->used = RT_FALSE;
        return ret;
    }
    job->head_len = g2d_cmdlist_finish(&job->cl);
    rt_mq_send(&g2d_queue, &job, sizeof(job));
    return 0;
}

int g2d_get_capabilities(struct g2d_capabilities *caps)
{
    caps->src_formats = BIT(G2D_PIXFMT_MAX) - 1U;
    caps->dst_formats = BIT(G2D_PIXFMT_NV12) - 1U;
    caps->ops = BIT(G2D_OP_FILL) | BIT(G2D_OP_BLIT) | BIT(G2D_OP_BLEND);
    caps->max_width = MAX_SIZE;
    caps->max_height = MAX_SIZE;
    caps->rotate_addr_align = 4;
    caps->rotate_pitch_align = 8;
    caps->queue_depth = QUEUE_DEPTH;
    return 0;
}

rt_bool_t g2d_ready(void)
{
    return g2d_up;
}

/* ---- synchronous helpers ------------------------------------------------------- */

struct run_ctx {
    struct rt_semaphore done;
    volatile int state;         /* 0 waiting, 1 done, 2 abandoned */
    int status;
    g2d_callback_t callback;
    void *user_data;
};

static void run_callback(const struct g2d_op *op, int status, void *user_data)
{
    struct run_ctx *ctx = user_data;
    rt_base_t level;
    int old;

    if (ctx->callback != NULL)
        ctx->callback(op, status, ctx->user_data);
    ctx->status = status;
    level = rt_hw_interrupt_disable();
    old = ctx->state;
    if (old == 0)
        ctx->state = 1;
    rt_hw_interrupt_enable(level);
    if (old == 0) {
        rt_sem_release(&ctx->done);
    } else {
        rt_sem_detach(&ctx->done);
        rt_free(ctx);
    }
}

int g2d_run(const struct g2d_op *op, rt_int32_t timeout)
{
    struct g2d_op copy = *op;
    struct run_ctx *ctx = rt_malloc(sizeof(*ctx));
    rt_base_t level;
    int ret, old;

    if (ctx == NULL)
        return -ENOMEM;
    rt_sem_init(&ctx->done, "g2dr", 0, RT_IPC_FLAG_FIFO);
    ctx->state = 0;
    ctx->callback = op->callback;
    ctx->user_data = op->user_data;
    copy.callback = run_callback;
    copy.user_data = ctx;

    ret = g2d_submit(&copy);
    if (ret) {
        rt_sem_detach(&ctx->done);
        rt_free(ctx);
        return ret;
    }
    if (rt_sem_take(&ctx->done, timeout) != RT_EOK) {
        level = rt_hw_interrupt_disable();
        old = ctx->state;
        if (old == 0)
            ctx->state = 2;
        rt_hw_interrupt_enable(level);
        if (old == 0)
            return -EAGAIN;     /* the callback frees the context when it finally runs */
        rt_sem_take(&ctx->done, RT_WAITING_FOREVER);
    }
    ret = ctx->status;
    rt_sem_detach(&ctx->done);
    rt_free(ctx);
    return ret;
}

int g2d_fill(const struct g2d_surface *dst, const struct g2d_rect *dst_rect, uint32_t color)
{
    struct g2d_op op = { .type = G2D_OP_FILL, .dst = *dst, .dst_rect = *dst_rect, .color = color };

    return g2d_run(&op, RT_WAITING_FOREVER);
}

int g2d_blit(const struct g2d_surface *src, const struct g2d_rect *src_rect,
             const struct g2d_surface *dst, const struct g2d_rect *dst_rect, enum g2d_rotation rotation, uint32_t flags)
{
    struct g2d_op op = { .type = G2D_OP_BLIT, .flags = flags, .src = *src, .src_rect = *src_rect,
                         .dst = *dst, .dst_rect = *dst_rect, .rotation = rotation };

    return g2d_run(&op, RT_WAITING_FOREVER);
}

int g2d_blend(const struct g2d_surface *fg, const struct g2d_rect *fg_rect,
              const struct g2d_surface *bg, const struct g2d_rect *bg_rect, const struct g2d_surface *dst,
              const struct g2d_rect *dst_rect, const struct g2d_blend *blend, uint32_t flags)
{
    struct g2d_op op = { .type = G2D_OP_BLEND, .flags = flags, .src = *fg, .src_rect = *fg_rect, .bg = *bg,
                         .bg_rect = *bg_rect, .dst = *dst, .dst_rect = *dst_rect, .blend = *blend };

    return g2d_run(&op, RT_WAITING_FOREVER);
}

/* ---- platform driver ------------------------------------------------------------ */

static rt_err_t g2d_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct rt_clk *mod, *bus, *mbus;
    struct rt_reset_control *rst;
    rt_uint32_t rate = 300000000u;
    int irq;
    rt_thread_t t;

    g2d_base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    irq = rt_dm_dev_get_irq(dev, 0);
    mod = rt_clk_get_by_name(dev, "mod");
    bus = rt_clk_get_by_name(dev, "bus");
    mbus = rt_clk_get_by_name(dev, "mbus");
    rst = rt_reset_control_get_by_index(dev, 0);
    if (!g2d_base || irq < 0 || rt_is_err_or_null(mod) || rt_is_err_or_null(bus) ||
        rt_is_err_or_null(mbus) || rt_is_err_or_null(rst))
        return -RT_ERROR;
    rt_dm_dev_prop_read_u32(dev, "clock-frequency", &rate);

    /* bus gate, memory bus gate and reset pulse, then the module clock */
    rt_reset_control_assert(rst);
    rt_clk_prepare_enable(bus);
    rt_clk_prepare_enable(mbus);
    rt_reset_control_deassert(rst);
    rt_clk_set_rate(mod, rate);
    rt_clk_prepare_enable(mod);
    g2d_mod_clk = mod;

    rt_sem_init(&hw_done, "g2d", 0, RT_IPC_FLAG_FIFO);
    rt_mq_init(&g2d_queue, "g2dq", g2d_queue_pool, sizeof(struct g2d_job *), sizeof(g2d_queue_pool), RT_IPC_FLAG_FIFO);
    g2d_hw_open();
    rt_pic_attach_irq(irq, g2d_isr, RT_NULL, "g2d", 0);
    rt_pic_irq_unmask(irq);

    t = rt_thread_create("g2d", g2d_thread, RT_NULL, 4096, 5, 10);
    if (t == RT_NULL)
        return -RT_ENOMEM;
    rt_thread_startup(t);
    g2d_up = RT_TRUE;
    LOG_I("ready, module clock %u Hz, queue depth %u", (rt_uint32_t)rt_clk_get_rate(mod), QUEUE_DEPTH);
    return RT_EOK;
}

static const struct rt_ofw_node_id g2d_ofw_ids[] =
{
    { .compatible = "allwinner,sunxi-g2d" },
    { /* sentinel */ }
};

static struct rt_platform_driver g2d_driver =
{
    .name = "g2d-sun252i",
    .ids = g2d_ofw_ids,
    .probe = g2d_probe,
};
RT_PLATFORM_DRIVER_EXPORT(g2d_driver);

/* hardware time of the last job in microseconds x 100 (from the start of the command list to the end interrupt) */
uint32_t g2d_last_hw_time_x100us(void)
{
    return (uint32_t)(hw_ticks * 100000000ull / rt_clock_time_get_freq());
}

/* The memory bandwidth limit of the G2D itself (0: no limit, otherwise the level of the register). */
void g2d_set_ddr_limit(uint32_t level)
{
    if (!g2d_up)
        return;
    g2d_upd(G2D_DDR_LIMIT, G2D_DDR_LIMIT_EN | G2D_DDR_LIMIT_MASK,
            level ? (G2D_DDR_LIMIT_EN | (level & G2D_DDR_LIMIT_MASK)) : 0);
}

uint32_t g2d_get_ddr_limit(void)
{
    uint32_t v = g2d_up ? g2d_rd(G2D_DDR_LIMIT) : 0;

    return (v & G2D_DDR_LIMIT_EN) ? (v & G2D_DDR_LIMIT_MASK) : 0;
}

/* The module clock of the G2D: request a rate, returns the rate set (0 on failure). */
uint32_t g2d_set_clock_rate(uint32_t hz)
{
    if (!g2d_up || rt_clk_set_rate(g2d_mod_clk, hz) != RT_EOK)
        return 0;

    return (uint32_t)rt_clk_get_rate(g2d_mod_clk);
}

uint32_t g2d_get_clock_rate(void)
{
    return g2d_up ? (uint32_t)rt_clk_get_rate(g2d_mod_clk) : 0;
}
