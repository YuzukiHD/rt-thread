/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fuses are read word by word through a small command interface: the word
 * index goes to the address register, a read command (with the key that guards
 * the register) to the control register, which clears the start bit when the
 * data is in the read data register. The chip identifier is the first 16 bytes.
 * msh command "sid_chipid" prints it.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "sid-sun252i.h"

#define SID_PRCTL       0x00u
#define SID_PR_ADDR     0x04u
#define SID_RDKEY       0x0cu

#define SID_OP_LOCK_SHIFT 16
#define SID_OP_LOCK_MASK  (0xffffu << SID_OP_LOCK_SHIFT)
#define SID_READ_OP_LOCK  0xadbfu
#define SID_READ_START    (1u << 1)
#define SID_START_MASK    ((1u << 0) | (1u << 1))
#define SID_INDEX_MASK    0xfu

static rt_ubase_t sid_base;
static struct rt_spinlock sid_lock;

static rt_uint32_t srd(rt_uint32_t off)
{
    return HWREG32(sid_base + off);
}
static void swr(rt_uint32_t off, rt_uint32_t v)
{
    HWREG32(sid_base + off) = v;
}

static int sid_read_word(unsigned int index, rt_uint32_t *val)
{
    rt_base_t level = rt_spin_lock_irqsave(&sid_lock);
    rt_uint32_t reg;
    int tries = 100000;

    reg = srd(SID_PR_ADDR);
    reg &= ~SID_INDEX_MASK;
    reg |= index & SID_INDEX_MASK;
    swr(SID_PR_ADDR, reg);

    reg = srd(SID_PRCTL);
    reg &= ~(SID_OP_LOCK_MASK | SID_START_MASK);
    reg |= (SID_READ_OP_LOCK << SID_OP_LOCK_SHIFT) | SID_READ_START;
    swr(SID_PRCTL, reg);

    while (srd(SID_PRCTL) & SID_READ_START)
    {
        if (--tries == 0)
        {
            rt_spin_unlock_irqrestore(&sid_lock, level);
            return -RT_ETIMEOUT;
        }
    }
    reg &= ~(SID_OP_LOCK_MASK | SID_START_MASK);
    swr(SID_PRCTL, reg);
    *val = srd(SID_RDKEY);
    rt_spin_unlock_irqrestore(&sid_lock, level);

    return 0;
}

rt_ssize_t sun252i_sid_read(rt_uint32_t offset, void *buf, rt_size_t len)
{
    rt_uint8_t *out = buf;
    rt_size_t i;

    if (!sid_base)
    {
        return -RT_ERROR;
    }
    if (((offset | len) % 4u) != 0u || offset + len > SUN252I_SID_BITS / 8)
    {
        return -RT_EINVAL;
    }
    for (i = 0; i < len; i += 4)
    {
        rt_uint32_t w;
        int ret = sid_read_word((offset + i) / 4, &w);

        if (ret != 0)
        {
            return ret;
        }
        memcpy(out + i, &w, 4);
    }

    return len;
}

rt_ssize_t sun252i_sid_chipid(rt_uint8_t *buf, rt_size_t len)
{
    rt_uint8_t id[SUN252I_SID_CHIPID_BYTES];
    rt_ssize_t ret = sun252i_sid_read(0, id, sizeof(id));

    if (ret < 0)
    {
        return ret;
    }
    len = len < sizeof(id) ? len : sizeof(id);
    memcpy(buf, id, len);

    return len;
}

static int sid_chipid(int argc, char **argv)
{
    rt_uint8_t id[SUN252I_SID_CHIPID_BYTES];
    rt_ssize_t n = sun252i_sid_chipid(id, sizeof(id));
    int i;

    if (n < 0)
    {
        rt_kprintf("sid: read failed (%d)\n", (int)n);
        return -1;
    }
    rt_kprintf("0x");
    for (i = 0; i < n; i++)
    {
        rt_kprintf("%02x", id[i]);
    }
    rt_kprintf("\n");
    return 0;
}
MSH_CMD_EXPORT(sid_chipid, print the 128 bit chip identifier);

static rt_err_t sid_probe(struct rt_platform_device *pdev)
{
    sid_base = (rt_ubase_t)rt_dm_dev_iomap(&pdev->parent, 0);
    if (!sid_base)
    {
        return -RT_EIO;
    }
    rt_spin_lock_init(&sid_lock);

    return RT_EOK;
}

static const struct rt_ofw_node_id sid_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-sid" },
    { /* sentinel */ }
};

static struct rt_platform_driver sid_driver =
{
    .name = "sid-sun252i",
    .ids = sid_ofw_ids,
    .probe = sid_probe,
};
RT_PLATFORM_DRIVER_EXPORT(sid_driver);
