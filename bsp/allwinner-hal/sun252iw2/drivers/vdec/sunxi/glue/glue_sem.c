/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Counting semaphores handed to the archive as opaque pointers */

#include <rtthread.h>

void *hal_sem_create(unsigned int cnt)
{
    return rt_sem_create("vesem", cnt, RT_IPC_FLAG_PRIO);
}

int hal_sem_delete(void *sem)
{
    return rt_sem_delete((rt_sem_t)sem) == RT_EOK ? 0 : -1;
}

int hal_sem_post(void *sem)
{
    rt_sem_release((rt_sem_t)sem);
    return 0;
}

/* The archive counts in milliseconds; the largest value means forever. */
int hal_sem_timedwait(void *sem, unsigned long ms)
{
    rt_int32_t to = (ms >= 0xffffffffUL) ? RT_WAITING_FOREVER : (rt_int32_t)rt_tick_from_millisecond((rt_int32_t)ms);

    return rt_sem_take((rt_sem_t)sem, to) == RT_EOK ? 0 : -1;
}

int hal_sem_getvalue(void *sem, int *val)
{
    *val = (int)((rt_sem_t)sem)->value;
    return 0;
}
