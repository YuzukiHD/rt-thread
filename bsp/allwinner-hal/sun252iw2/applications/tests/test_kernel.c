/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Kernel basics: threads, IPC objects, timers, delay accuracy, tick rate,
 * heap and the trap path.
 */
#include <rtthread.h>
#include <rthw.h>
#include "test.h"

static volatile int order[4];
static volatile int order_n;

static void prio_entry(void *p)
{
    order[order_n++] = (int)(rt_ubase_t)p;
}

static void test_threads(struct test_ctx *c)
{
    rt_thread_t lo, hi;

    order_n = 0;
    lo = rt_thread_create("lo", prio_entry, (void *)1, 2048, 12, 5);
    hi = rt_thread_create("hi", prio_entry, (void *)2, 2048, 8, 5);
    /* the lower one starts first but the higher one must run first */
    rt_enter_critical();
    rt_thread_startup(lo);
    rt_thread_startup(hi);
    rt_exit_critical();
    rt_thread_mdelay(50);
    TEST_CHECK(c, order_n == 2 && order[0] == 2 && order[1] == 1, "thread priority order");
}

static rt_sem_t sem;
static rt_mutex_t mtx;
static rt_mailbox_t mb;
static rt_mq_t mq;
static volatile int shared;

static void prod(void *p)
{
    rt_sem_release(sem);
    rt_mb_send(mb, 0x1234);
    int v = 77;
    rt_mq_send(mq, &v, sizeof(v));
}

static void mutex_entry(void *p)
{
    int i;
    for (i = 0; i < 1000; i++)
    {
        rt_mutex_take(mtx, RT_WAITING_FOREVER);
        int t = shared;
        if ((i & 15) == 0) rt_thread_yield();
        shared = t + 1;
        rt_mutex_release(mtx);
    }
    rt_sem_release(sem);
}

static void test_ipc(struct test_ctx *c)
{
    rt_ubase_t m;
    int v = 0;

    sem = rt_sem_create("s", 0, RT_IPC_FLAG_PRIO);
    mtx = rt_mutex_create("m", RT_IPC_FLAG_PRIO);
    mb = rt_mb_create("mb", 4, RT_IPC_FLAG_PRIO);
    mq = rt_mq_create("mq", sizeof(int), 4, RT_IPC_FLAG_PRIO);

    rt_thread_startup(rt_thread_create("prod", prod, RT_NULL, 2048, 8, 5));
    TEST_CHECK(c, rt_sem_take(sem, 200) == RT_EOK, "semaphore");
    TEST_CHECK(c, rt_mb_recv(mb, &m, 200) == RT_EOK && m == 0x1234, "mailbox");
    TEST_CHECK(c, rt_mq_recv(mq, &v, sizeof(v), 200) > 0 && v == 77, "message queue");
    TEST_CHECK(c, rt_sem_take(sem, 20) == -RT_ETIMEOUT, "semaphore timeout");

    shared = 0;
    rt_thread_startup(rt_thread_create("m1", mutex_entry, RT_NULL, 2048, 12, 2));
    rt_thread_startup(rt_thread_create("m2", mutex_entry, RT_NULL, 2048, 12, 2));
    rt_sem_take(sem, RT_WAITING_FOREVER);
    rt_sem_take(sem, RT_WAITING_FOREVER);
    TEST_CHECK(c, shared == 2000, "mutex protected counter");

    rt_sem_delete(sem); rt_mutex_delete(mtx); rt_mb_delete(mb); rt_mq_delete(mq);
}

static volatile int tmr_n;
static void tmr_cb(void *p) { tmr_n++; }

static void test_timer(struct test_ctx *c)
{
    rt_timer_t t = rt_timer_create("t", tmr_cb, RT_NULL, 1, RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    tmr_n = 0;
    rt_timer_start(t);
    rt_thread_mdelay(1010);
    rt_timer_stop(t);
    rt_timer_delete(t);
    rt_kprintf("soft timer fired %d times\n", tmr_n);
    TEST_CHECK(c, tmr_n >= 98 && tmr_n <= 101, "soft timer 1 tick x 1 s");
}

static void test_tick(struct test_ctx *c)
{
    rt_uint32_t t0, t1;
    rt_tick_t k0, k1;
    rt_uint32_t us;

    k0 = rt_tick_get();
    t0 = test_mtime();
    rt_thread_mdelay(5000);
    t1 = test_mtime();
    k1 = rt_tick_get();
    us = (t1 - t0) / 24;
    rt_kprintf("5000 ms delay = %u us, %u ticks\n", us, (rt_uint32_t)(k1 - k0));
    TEST_CHECK(c, us >= 4980000 && us <= 5030000, "mdelay(5000) within 0.5 percent of mtime");
    TEST_CHECK(c, (k1 - k0) >= 499 && (k1 - k0) <= 501, "100 ticks per second");
}

static void test_heap(struct test_ctx *c)
{
    rt_size_t total, used, maxused, used1;
    void *p[16];
    int i, ok = 1;

    rt_memory_info(&total, &used, &maxused);
    for (i = 0; i < 200; i++)
    {
        int j;
        for (j = 0; j < 16; j++)
        {
            p[j] = rt_malloc(100 + j * 37);
            if (!p[j]) ok = 0; else rt_memset(p[j], 0xA5, 100 + j * 37);
        }
        for (j = 0; j < 16; j++) rt_free(p[j]);
    }
    rt_memory_info(&total, &used1, &maxused);
    TEST_CHECK(c, ok, "malloc/free loop");
    TEST_CHECK(c, used1 == used, "heap used unchanged after loop");
    rt_kprintf("heap total=%u used=%u\n", (rt_uint32_t)total, (rt_uint32_t)used1);
}

static int test_kernel(int argc, char **argv)
{
    struct test_ctx c = {0, 0};

    test_threads(&c);
    test_ipc(&c);
    test_timer(&c);
    test_tick(&c);
    test_heap(&c);
    return test_summary("kernel", &c);
}
MSH_CMD_EXPORT(test_kernel, kernel threads ipc timer tick heap);

/* the trap path: an illegal instruction must stop with a report (cause 2) */
static int test_trap(int argc, char **argv)
{
    rt_kprintf("triggering an illegal instruction, the board stops with EXC...\n");
    rt_thread_mdelay(50);
    __asm__ volatile(".word 0"); /* illegal instruction */
    return 0;
}
MSH_CMD_EXPORT(test_trap, trigger an illegal instruction);
