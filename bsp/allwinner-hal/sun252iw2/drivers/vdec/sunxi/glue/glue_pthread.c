/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Mutexes for the archive. Its pthread_mutex_t is a 40 byte block that is all
 * zero when idle; the first word holds the address of an rt_mutex that is
 * created on the first lock or init.
 */

#include <stdint.h>
#include <rtthread.h>
#include <rthw.h>

static rt_mutex_t mutex_of(uint32_t *m)
{
    rt_mutex_t mu = (rt_mutex_t)(uintptr_t)*m;

    if (mu == RT_NULL)
    {
        rt_base_t level = rt_hw_interrupt_disable();

        mu = (rt_mutex_t)(uintptr_t)*m;
        rt_hw_interrupt_enable(level);
        if (mu == RT_NULL)
        {
            mu = rt_mutex_create("vemtx", RT_IPC_FLAG_PRIO);
            level = rt_hw_interrupt_disable();
            if (*m == 0 && mu != RT_NULL)
                *m = (uint32_t)(uintptr_t)mu;
            else if (mu != RT_NULL)
            {
                /* somebody else was faster */
                rt_hw_interrupt_enable(level);
                rt_mutex_delete(mu);
                level = rt_hw_interrupt_disable();
                mu = (rt_mutex_t)(uintptr_t)*m;
            }
            rt_hw_interrupt_enable(level);
        }
    }

    return mu;
}

int pthread_mutex_init(uint32_t *m, const void *attr)
{
    (void)attr;
    *m = 0;

    return mutex_of(m) != RT_NULL ? 0 : -1;
}

int pthread_mutex_lock(uint32_t *m)
{
    rt_mutex_t mu = mutex_of(m);

    return (mu != RT_NULL && rt_mutex_take(mu, RT_WAITING_FOREVER) == RT_EOK) ? 0 : -1;
}

int pthread_mutex_unlock(uint32_t *m)
{
    rt_mutex_t mu = mutex_of(m);

    return (mu != RT_NULL && rt_mutex_release(mu) == RT_EOK) ? 0 : -1;
}

int pthread_mutex_destroy(uint32_t *m)
{
    if (*m != 0)
        rt_mutex_delete((rt_mutex_t)(uintptr_t)*m);
    *m = 0;

    return 0;
}
