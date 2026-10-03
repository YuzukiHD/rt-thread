/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FPU (F+D): every worker keeps its own float/double state in registers and
 * yields / gets preempted by the tick between the operations; all results are
 * exactly representable so they must match bit for bit.
 */
#include <rtthread.h>
#include <math.h>
#include "test.h"

#define CSR_FFLAGS_READ() ({ rt_ubase_t v; __asm__ volatile("frflags %0" : "=r"(v)); v; })
#define CSR_FRM_READ()    ({ rt_ubase_t v; __asm__ volatile("frrm %0" : "=r"(v)); v; })

struct worker
{
    float fstep;
    double dstep;
    int n;
    int rm;            /* rounding mode this worker sets (0 RNE, 1 RTZ, 2 RDN) */
    volatile int done;
    int ok;
    int frm_ok;
    rt_sem_t sem;
};

static void worker_entry(void *p)
{
    struct worker *w = (struct worker *)p;
    float f = 0.0f;
    double d = 0.0;
    float g = 1.0f;
    int i;

    w->ok = 1;
    w->frm_ok = 1;
    __asm__ volatile("fsrm %0, %1" : "=r"(i) : "r"(w->rm));
    for (i = 1; i <= w->n; i++)
    {
        f += w->fstep;
        d += w->dstep;
        g = g * 2.0f;
        if (g > 1048576.0f) g = 1.0f;
        if ((i & 7) == 0) rt_thread_yield();
        if ((float)(i) * w->fstep != f) w->ok = 0;
        if ((double)(i) * w->dstep != d) w->ok = 0;
        if (CSR_FRM_READ() != (rt_ubase_t)w->rm) w->frm_ok = 0;
    }
    /* sqrt of perfect squares is exact in every rounding mode */
    {
        double s = sqrt((double)(w->n) * (double)(w->n));
        if (s != (double)w->n) w->ok = 0;
        float sf = sqrtf((float)(w->n) * (float)(w->n));
        if (sf != (float)w->n) w->ok = 0;
    }
    rt_sem_release(w->sem);
}

static int test_fpu(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct worker w[3] = {
        {0.5f,   0.25,  3000, 0},
        {0.125f, 0.75,  3000, 1},
        {2.0f,   0.0625, 3000, 2},
    };
    rt_sem_t sem = rt_sem_create("fpu", 0, RT_IPC_FLAG_PRIO);
    int i;

    /* all three become ready together so they really interleave */
    rt_enter_critical();
    for (i = 0; i < 3; i++)
    {
        w[i].sem = sem;
        rt_thread_startup(rt_thread_create("fpuw", worker_entry, &w[i], 2048, 12, 1));
    }
    rt_exit_critical();
    for (i = 0; i < 3; i++) rt_sem_take(sem, RT_WAITING_FOREVER);
    rt_sem_delete(sem);

    TEST_CHECK(&c, w[0].ok, "float/double state thread 0 (preempted)");
    TEST_CHECK(&c, w[1].ok, "float/double state thread 1 (preempted)");
    TEST_CHECK(&c, w[2].ok, "float/double state thread 2 (preempted)");
    TEST_CHECK(&c, w[0].frm_ok && w[1].frm_ok && w[2].frm_ok, "per-thread rounding mode kept over switches");

    /* flags: divide by zero, invalid, inexact */
    {
        volatile float zero = 0.0f, one = 1.0f, three = 3.0f, r;
        rt_ubase_t fl;
        __asm__ volatile("fsflags zero");
        r = one / zero;
        fl = CSR_FFLAGS_READ();
        TEST_CHECK(&c, (fl & 0x08) && isinf(r), "divide by zero sets DZ, result inf");
        __asm__ volatile("fsflags zero");
        r = zero / zero;
        fl = CSR_FFLAGS_READ();
        TEST_CHECK(&c, (fl & 0x10) && isnan(r), "0/0 sets NV, result NaN");
        __asm__ volatile("fsflags zero");
        r = one / three;
        fl = CSR_FFLAGS_READ();
        TEST_CHECK(&c, (fl & 0x01) != 0, "1/3 sets NX");
        TEST_CHECK(&c, r > 0.33333f && r < 0.33334f, "1/3 value");
    }

    /* conversions and bit patterns */
    {
        volatile float f = 1.5f;
        volatile double d = -2.25;
        union { float f; rt_uint32_t u; } uf = { f };
        union { double d; rt_uint64_t u; } ud = { d };
        TEST_CHECK(&c, uf.u == 0x3fc00000u, "float 1.5 bit pattern");
        TEST_CHECK(&c, ud.u == 0xc002000000000000ull, "double -2.25 bit pattern");
        TEST_CHECK(&c, (int)(d * 4.0) == -9 && (int)(f * 2.0f) == 3, "int conversion");
        TEST_CHECK(&c, (double)f == 1.5 && (float)d == -2.25f, "float <-> double");
    }

    /* libm */
    {
        volatile double x = 1.0;
        double s = sin(x), co = cos(x);
        rt_int32_t sm = (rt_int32_t)(s * 1000000.0), cm = (rt_int32_t)(co * 1000000.0);
        rt_kprintf("sin(1)=%d e-6 cos(1)=%d e-6\n", sm, cm);
        TEST_CHECK(&c, sm == 841470 && cm == 540302, "sin/cos(1) libm");
        TEST_CHECK(&c, (rt_int32_t)(exp(1.0) * 100000.0) == 271828, "exp(1) libm");
    }
    return test_summary("fpu", &c);
}
MSH_CMD_EXPORT(test_fpu, FPU float/double context test);
