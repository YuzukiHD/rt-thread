/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small check helpers of the msh test commands (test_*): every test prints
 * one "[PASS]"/"[FAIL]" line per check and a summary the host greps for.
 */
#ifndef __TEST_H__
#define __TEST_H__

#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/ofw.h>
#include <drivers/ofw_io.h>

struct test_ctx
{
    int pass;
    int fail;
};

#define TEST_CHECK(ctx, cond, name)                                          \
    do {                                                                     \
        if (cond) { (ctx)->pass++; rt_kprintf("[PASS] %s\n", name); }        \
        else      { (ctx)->fail++; rt_kprintf("[FAIL] %s (%s:%d)\n", name, __FILE__, __LINE__); } \
    } while (0)

static inline int test_summary(const char *name, struct test_ctx *ctx)
{
    rt_kprintf("TEST %s: %s (pass=%d fail=%d)\n", name,
               ctx->fail ? "FAIL" : "PASS", ctx->pass, ctx->fail);
    return ctx->fail ? -1 : 0;
}

/* registers of a device tree node (read back checks of tests that have no other observable) */
static inline volatile rt_uint32_t *test_node_regs(const char *path)
{
    struct rt_ofw_node *np = rt_ofw_find_node_by_path(path);

    return np ? (volatile rt_uint32_t *)rt_ofw_iomap(np, 0) : RT_NULL;
}

/* free-running 24 MHz counter (CLINT mtime, low word) */
static inline rt_uint32_t test_mtime(void)
{
    return *(volatile rt_uint32_t *)0x1400BFF8u;
}

#endif
