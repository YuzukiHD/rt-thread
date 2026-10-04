/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CPU clock: the rate reads back, can be raised by one step with the memory self test
 * passing and is lowered again (cpu_freq). The original rate is restored.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "sunxi-util.h"
#include "test.h"

extern int msh_exec(char *cmd, rt_size_t length);

static int test_cpu(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_ubase_t orig = sun252i_cpu_get_rate(), got;
    char cmd[24];

    rt_kprintf("cpu: running at %u MHz\n", (rt_uint32_t)(orig / 1000000u));
    TEST_CHECK(&c, orig >= 24000000u && orig <= 1008000000u, "rate reads back in range");

    got = sun252i_cpu_raise_rate(orig + 72000000u);
    rt_kprintf("cpu: raised to %u MHz\n", (rt_uint32_t)(got / 1000000u));
    TEST_CHECK(&c, got == orig + 72000000u, "raised by 72 MHz, self test passed");

    rt_snprintf(cmd, sizeof(cmd), "cpu_freq %u", (rt_uint32_t)(orig / 1000000u));
    msh_exec(cmd, rt_strlen(cmd));
    TEST_CHECK(&c, sun252i_cpu_get_rate() == orig, "original rate restored");

    return test_summary("cpu", &c);
}
MSH_CMD_EXPORT(test_cpu, CPU clock rate read, raise and lower);
