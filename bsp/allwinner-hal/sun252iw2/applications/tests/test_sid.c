/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SID: the chip identifier is not zero and does not change between reads,
 * every fuse word can be read, unaligned and out of range reads are refused.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "sid-sun252i.h"
#include "test.h"

static int test_sid(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_uint8_t a[16], b[16], zero[16] = {0};
    rt_uint8_t all[SUN252I_SID_BITS / 8];
    int i;

    TEST_CHECK(&c, sun252i_sid_chipid(a, sizeof(a)) == 16, "chip id reads 16 bytes");
    rt_kprintf("sid: chip id 0x");
    for (i = 0; i < 16; i++)
        rt_kprintf("%02x", a[i]);
    rt_kprintf("\n");
    TEST_CHECK(&c, memcmp(a, zero, 16) != 0, "chip id is not zero");
    for (i = 0; i < 8; i++)
        sun252i_sid_chipid(b, sizeof(b));
    TEST_CHECK(&c, memcmp(a, b, 16) == 0, "chip id identical over repeated reads");
    TEST_CHECK(&c, sun252i_sid_read(0, all, sizeof(all)) == (rt_ssize_t)sizeof(all), "all 512 fuse bits read");
    TEST_CHECK(&c, memcmp(all, a, 16) == 0, "first four fuse words are the chip id");
    TEST_CHECK(&c, sun252i_sid_read(2, b, 4) < 0, "unaligned offset refused");
    TEST_CHECK(&c, sun252i_sid_read(sizeof(all), b, 4) < 0, "read past the end refused");

    return test_summary("sid", &c);
}
MSH_CMD_EXPORT(test_sid, SID chip id and fuse reads);
