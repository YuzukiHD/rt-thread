/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The capacitive touch panel (FT5x46 family, I2C address 0x38 on I2C1) as the touch screen of the USB display: a thread
 * polls the controller and turns its points into reports of the HID touch interface.
 *
 *   usb_ctp_scan            list the devices that answer on the bus
 *   usb_ctp_dump [n]        print n reads of the point registers (touch the panel)
 *   usb_ctp_swap [0|1]      exchange X and Y (and mirror them with usb_ctp_flip)
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>

#include "usb_touch.h"

#define CTP_BUS         "i2c1"
#define CTP_ADDR        0x38
#define CTP_MAX_X       1024
#define CTP_MAX_Y       600
#define CTP_POLL_MS     10

#define REG_TD_STATUS   0x02
#define POINT_BYTES     6
#define POINTS          USB_TOUCH_MAX_CONTACTS

static struct rt_i2c_bus_device *bus;
static rt_bool_t swap_xy, flip_x, flip_y;
static volatile rt_bool_t running;

static int ctp_read(rt_uint8_t reg, rt_uint8_t *buf, rt_size_t len)
{
    struct rt_i2c_msg m[2] = {
        { CTP_ADDR, RT_I2C_WR, 1, &reg },
        { CTP_ADDR, RT_I2C_RD, len, buf },
    };

    return rt_i2c_transfer(bus, m, 2) == 2 ? 0 : -1;
}

static rt_bool_t ctp_open(void)
{
    if (!bus)
        bus = rt_i2c_bus_device_find(CTP_BUS);
    if (!bus)
        rt_kprintf("usb ctp: no %s bus\n", CTP_BUS);

    return bus != RT_NULL;
}

/* the status register and the points behind it: [0] count, then 6 bytes per point (event+x high, x low, id+y high, y low, weight, area) */
static int ctp_points(rt_uint8_t *raw)
{
    return ctp_read(REG_TD_STATUS, raw, 1 + POINTS * POINT_BYTES);
}

static void scale(rt_uint32_t x, rt_uint32_t y, uint16_t *ox, uint16_t *oy)
{
    rt_uint32_t t;

    if (swap_xy)
    {
        t = x; x = y; y = t;
    }
    if (flip_x)
        x = CTP_MAX_X - 1 - x;
    if (flip_y)
        y = CTP_MAX_Y - 1 - y;
    if (x >= CTP_MAX_X)
        x = CTP_MAX_X - 1;
    if (y >= CTP_MAX_Y)
        y = CTP_MAX_Y - 1;
    *ox = x * USB_TOUCH_MAX / (CTP_MAX_X - 1);
    *oy = y * USB_TOUCH_MAX / (CTP_MAX_Y - 1);
}

static void ctp_thread(void *arg)
{
    struct usb_touch_point out[POINTS], last[POINTS];
    rt_bool_t was_down[POINTS];
    int reported = 0;
    rt_uint8_t raw[1 + POINTS * POINT_BYTES];

    memset(was_down, 0, sizeof(was_down));
    memset(last, 0, sizeof(last));
    running = RT_TRUE;
    while (running)
    {
        int i, n, count = 0, changed = 0;
        rt_bool_t down[POINTS];

        rt_thread_mdelay(CTP_POLL_MS);
        if (ctp_points(raw) != 0)
            continue;
        n = raw[0] & 0x0F;
        if (n > POINTS)
            n = 0;
        memset(down, 0, sizeof(down));
        memset(out, 0, sizeof(out));
        for (i = 0; i < n; i++)
        {
            const rt_uint8_t *p = &raw[1 + i * POINT_BYTES];
            int id = p[2] >> 4;
            rt_uint32_t x = ((p[0] & 0x0F) << 8) | p[1], y = ((p[2] & 0x0F) << 8) | p[3];

            if (id >= POINTS || (p[0] >> 6) == 1)   /* lifted */
                continue;
            down[id] = RT_TRUE;
            out[id].id = id;
            out[id].down = RT_TRUE;
            scale(x, y, &out[id].x, &out[id].y);
        }
        /* a finger that was down and is not now: one report with its tip switch off */
        for (i = 0; i < POINTS; i++)
        {
            if (!down[i] && was_down[i])
            {
                out[i] = last[i];
                out[i].down = RT_FALSE;
                changed = 1;
            }
            if (down[i] && (!was_down[i] || out[i].x != last[i].x || out[i].y != last[i].y))
                changed = 1;
            if (down[i] || was_down[i])
                count = i + 1;
        }
        if (!changed)
            continue;
        /* the report holds the slots 0..count-1; free slots are sent as not touching */
        for (i = 0; i < count; i++)
        {
            if (!down[i] && !was_down[i])
            {
                memset(&out[i], 0, sizeof(out[i]));
                out[i].id = i;
            }
        }
        while (usb_touch_send(out, count) == -RT_EBUSY && running)
            rt_thread_mdelay(1);
        reported++;
        for (i = 0; i < POINTS; i++)
        {
            was_down[i] = down[i];
            if (down[i])
                last[i] = out[i];
        }
    }
    (void)reported;
}

void usb_ctp_start(void)
{
    rt_thread_t t;

    if (running || !ctp_open())
        return;
    t = rt_thread_create("ctp", ctp_thread, RT_NULL, 2048, 9, 10);
    if (t)
        rt_thread_startup(t);
}

static int usb_ctp_scan(int argc, char **argv)
{
    int addr, found = 0, timeouts = 0, nacks = 0, other = 0;

    if (!ctp_open())
        return -1;
    for (addr = 0x08; addr < 0x78; addr++)
    {
        rt_uint8_t b;
        struct rt_i2c_msg m = { addr, RT_I2C_RD, 1, &b };

        rt_ssize_t r = rt_i2c_transfer(bus, &m, 1);

        if (r == 1)
        {
            rt_kprintf("usb ctp: device at 0x%02x\n", addr);
            found++;
        }
        else if (r == -RT_ETIMEOUT)
            timeouts++;
        else if (r == -RT_EIO || r == 0)
            nacks++;
        else
            other++;
    }
    rt_kprintf("usb ctp: %d device(s) on %s (%d timeouts, %d no answer, %d other)\n", found, CTP_BUS, timeouts, nacks, other);

    return 0;
}
MSH_CMD_EXPORT(usb_ctp_scan, list the devices on the touch panel bus);

static int usb_ctp_dump(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 100, i;
    rt_uint8_t raw[1 + POINTS * POINT_BYTES], id[3] = { 0 };

    if (!ctp_open())
        return -1;
    if (ctp_read(0xA3, id, 1) == 0)
        rt_kprintf("usb ctp: chip id 0x%02x\n", id[0]);
    for (i = 0; i < n; i++)
    {
        if (ctp_points(raw) != 0)
        {
            rt_kprintf("usb ctp: read failed\n");
            return -1;
        }
        if ((raw[0] & 0x0F) != 0)
        {
            int k;

            for (k = 0; k < (raw[0] & 0x0F) && k < POINTS; k++)
            {
                const rt_uint8_t *p = &raw[1 + k * POINT_BYTES];

                rt_kprintf("usb ctp: id %d event %d x %d y %d\n", p[2] >> 4, p[0] >> 6, ((p[0] & 0x0F) << 8) | p[1], ((p[2] & 0x0F) << 8) | p[3]);
            }
        }
        rt_thread_mdelay(20);
    }

    return 0;
}
MSH_CMD_EXPORT(usb_ctp_dump, print the points the touch panel reports: usb_ctp_dump [reads]);

static int usb_ctp_orient(int argc, char **argv)
{
    /* usb_ctp_orient swap flipx flipy */
    if (argc > 3)
    {
        swap_xy = atoi(argv[1]);
        flip_x = atoi(argv[2]);
        flip_y = atoi(argv[3]);
    }
    rt_kprintf("usb ctp: swap %d, flip x %d, flip y %d\n", swap_xy, flip_x, flip_y);

    return 0;
}
MSH_CMD_EXPORT(usb_ctp_orient, orientation of the touch panel: usb_ctp_orient swap flipx flipy);

/* usb_ctp_pins: the levels of the two bus lines as GPIO inputs, without a pull, with pull-up and pull-down (the I2C mux is gone until reboot) */
static int usb_ctp_pins(int argc, char **argv)
{
    static const char *const names[] = { "floating", "pull-up", "pull-down" };
    static const rt_uint8_t modes[] = { PIN_MODE_INPUT, PIN_MODE_INPUT_PULLUP, PIN_MODE_INPUT_PULLDOWN };
    int bank = argc > 1 ? atoi(argv[1]) : 4, a = argc > 2 ? atoi(argv[2]) : 0, b = argc > 3 ? atoi(argv[3]) : 1, i;

    for (i = 0; i < 3; i++)
    {
        rt_pin_mode(bank * 32 + a, modes[i]);
        rt_pin_mode(bank * 32 + b, modes[i]);
        rt_thread_mdelay(5);
        rt_kprintf("usb ctp: P%c%d=%d P%c%d=%d (%s)\n", 'A' + bank, a, rt_pin_read(bank * 32 + a), 'A' + bank, b,
                   rt_pin_read(bank * 32 + b), names[i]);
    }

    return 0;
}
MSH_CMD_EXPORT(usb_ctp_pins, levels of the bus lines as GPIO: usb_ctp_pins [bank a b]);

/* usb_ctp_unstick: nine clock pulses on SCL (a slave that holds SDA low lets go after them), SDA is read after each */
static int usb_ctp_unstick(int argc, char **argv)
{
    int scl = 4 * 32 + 0, sda = 4 * 32 + 1, i;

    rt_pin_mode(sda, PIN_MODE_INPUT);
    rt_pin_mode(scl, PIN_MODE_OUTPUT_OD);
    rt_pin_write(scl, 1);
    rt_kprintf("usb ctp: SDA before %d\n", rt_pin_read(sda));
    for (i = 0; i < 9; i++)
    {
        rt_pin_write(scl, 0);
        rt_thread_mdelay(1);
        rt_pin_write(scl, 1);
        rt_thread_mdelay(1);
        rt_kprintf("usb ctp: clock %d, SCL %d SDA %d\n", i + 1, rt_pin_read(scl), rt_pin_read(sda));
    }

    return 0;
}
MSH_CMD_EXPORT(usb_ctp_unstick, clock the bus lines by hand to release a stuck slave);
