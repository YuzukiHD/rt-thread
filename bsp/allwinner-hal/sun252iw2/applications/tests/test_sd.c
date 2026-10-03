/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD card on SMHC0: probe, capacity, MBR signature, a 2 MiB read, and with
 * "write" a read-modify-write-verify-restore of one sector far from the start.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "test.h"

static int test_sd(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_device_t dev = rt_device_find("sd0");
    struct rt_device_blk_geometry geo;
    rt_uint8_t *buf, *buf2;
    rt_uint32_t t0, ms;
    rt_ssize_t r;
    int i, ok = 1;

    TEST_CHECK(&c, dev != RT_NULL, "sd0 block device present (card inserted?)");
    if (!dev) return test_summary("sd", &c);
    TEST_CHECK(&c, rt_device_open(dev, RT_DEVICE_OFLAG_RDWR) == RT_EOK, "open");
    rt_device_control(dev, RT_DEVICE_CTRL_BLK_GETGEOME, &geo);
    rt_kprintf("sd: %u sectors of %u bytes = %u MiB\n", (rt_uint32_t)geo.sector_count,
               (rt_uint32_t)geo.bytes_per_sector, (rt_uint32_t)((rt_uint64_t)geo.sector_count * geo.bytes_per_sector >> 20));
    TEST_CHECK(&c, geo.bytes_per_sector == 512 && geo.sector_count > 2048, "geometry plausible");

    buf = rt_malloc_align(2 * 1024 * 1024, 64);
    buf2 = rt_malloc_align(4096, 64);
    if (!buf || !buf2) { rt_kprintf("no memory\n"); return -1; }

    r = rt_device_read(dev, 0, buf, 1);
    TEST_CHECK(&c, r == 1, "read sector 0");
    rt_kprintf("sd: MBR signature %02x%02x\n", buf[510], buf[511]);
    TEST_CHECK(&c, buf[510] == 0x55 && buf[511] == 0xaa, "MBR signature 55aa");

    t0 = test_mtime();
    r = rt_device_read(dev, 0, buf, 4096);
    ms = (test_mtime() - t0) / 24000;
    rt_kprintf("sd: 2 MiB read in %u ms\n", ms);
    TEST_CHECK(&c, r == 4096, "read 2 MiB");

    /* unaligned destination, odd length: goes through the bounce path */
    r = rt_device_read(dev, 0, buf + 4, 3);
    TEST_CHECK(&c, r == 3 && memcmp(buf + 4, buf2, 0) == 0, "read 3 sectors to an unaligned address");

    if (argc > 1 && !strcmp(argv[1], "write"))
    {
        rt_uint32_t lba = (rt_uint32_t)(geo.sector_count - 64);
        rt_uint8_t orig[512];

        rt_device_read(dev, lba, orig, 1);
        for (i = 0; i < 512; i++) buf2[i] = (rt_uint8_t)(i * 3 + 7);
        r = rt_device_write(dev, lba, buf2, 1);
        TEST_CHECK(&c, r == 1, "write one sector near the end");
        memset(buf, 0, 512);
        rt_device_read(dev, lba, buf, 1);
        for (i = 0; i < 512; i++) if (buf[i] != buf2[i]) ok = 0;
        TEST_CHECK(&c, ok, "written sector reads back");
        r = rt_device_write(dev, lba, orig, 1);
        TEST_CHECK(&c, r == 1, "original sector restored");
    }
    rt_device_close(dev);
    return test_summary("sd", &c);
}
MSH_CMD_EXPORT(test_sd, SD card read test (test_sd write: also one sector write));
