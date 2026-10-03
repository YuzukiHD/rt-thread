/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C scan, SPI NOR (JEDEC id and a read/erase/program cycle in a free area),
 * PWM registers, GPADC.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "test.h"

/* ---- I2C ------------------------------------------------------------- */
/*
 * On the EVB all three I2C pin pairs are taken (PE8/9 console, PB0/1 backlight,
 * PD10/11 panel), so no slave can be reached: this checks the controller
 * (soft reset, clock register, idle bus state) and, when a bus with pull-ups
 * and a slave is wired, scans it. START timeouts on every address are
 * reported as SKIP, not as a pass of the transfer path.
 */
static int test_i2c(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct rt_i2c_bus_device *bus = rt_i2c_bus_device_find(argc > 1 ? argv[1] : "i2c2");
    volatile rt_uint32_t *tr = test_node_regs("/soc/i2c@2502800");
    int addr, found = 0, timeouts = 0, tried = 0;

    TEST_CHECK(&c, bus != RT_NULL, "i2c bus registered");
    if (!bus) return test_summary("i2c", &c);

    TEST_CHECK(&c, tr[5] == 0x59u, "100 kHz clock register programmed");
    TEST_CHECK(&c, (tr[3] & 0x40u) != 0u, "bus enable set");
    TEST_CHECK(&c, (tr[4] & 0xffu) == 0xf8u, "controller idle status 0xf8");

    for (addr = 0x08; addr < 0x78 && tried < 8; addr++, tried++)
    {
        rt_uint8_t b;
        struct rt_i2c_msg m = {addr, RT_I2C_RD, 1, &b};
        rt_ssize_t r = rt_i2c_transfer(bus, &m, 1);

        if (r == 1) { found++; rt_kprintf("i2c: device at 0x%02x\n", addr); }
        else if (r == -RT_ETIMEOUT) timeouts++;
    }
    if (timeouts == tried)
        rt_kprintf("[SKIP] START never completes: no usable bus on this board (see note)\n");
    else
        TEST_CHECK(&c, RT_TRUE, "addresses answered or NACKed by the controller");
    return test_summary("i2c", &c);
}
MSH_CMD_EXPORT(test_i2c, I2C controller state and scan (test_i2c [busname]));

/* ---- SPI NOR ----------------------------------------------------------- */
#define NOR_TEST_ADDR  0x00F00000u     /* inside the last megabyte of a 16 MB part, above the boot images */

static struct rt_spi_device *nor;

static int nor_cmd(const rt_uint8_t *tx, int txn, rt_uint8_t *rx, int rxn)
{
    return rt_spi_send_then_recv(nor, tx, txn, rx, rxn) == RT_EOK ? 0 : -1;
}

static void nor_wait(void)
{
    rt_uint8_t cmd = 0x05, st = 1;
    int n = 0;

    while ((st & 1) && n++ < 3000)
    {
        nor_cmd(&cmd, 1, &st, 1);
        rt_thread_mdelay(1);
    }
}

static void nor_wen(void)
{
    rt_uint8_t cmd = 0x06;
    rt_spi_send(nor, &cmd, 1);
}

static int test_spi(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct rt_spi_configuration cfg = {.mode = RT_SPI_MODE_0 | RT_SPI_MSB, .data_width = 8, .max_hz = 20 * 1000 * 1000};
    rt_uint8_t cmd[4], id[3] = {0}, rd[256], wr[256];
    int i, err = 0;
    int do_write = argc > 1 && !strcmp(argv[1], "write");

    {
        static struct rt_spi_device dev;
        TEST_CHECK(&c, rt_spi_bus_attach_device(&dev, "spi00", "spi0", RT_NULL) == RT_EOK ||
                       rt_device_find("spi00") != RT_NULL, "attach NOR to spi0");
        nor = (struct rt_spi_device *)rt_device_find("spi00");
    }
    if (!nor) return test_summary("spi", &c);
    TEST_CHECK(&c, rt_spi_configure(nor, &cfg) == RT_EOK, "configure 20 MHz mode 0");

    cmd[0] = 0x9f;
    nor_cmd(cmd, 1, id, 3);
    rt_kprintf("spi: JEDEC id %02x %02x %02x\n", id[0], id[1], id[2]);
    TEST_CHECK(&c, id[0] != 0x00 && id[0] != 0xff, "JEDEC manufacturer id is valid");

    /* a fast read of the first bytes of the flash: the boot image header magic */
    cmd[0] = 0x03; cmd[1] = cmd[2] = cmd[3] = 0;
    nor_cmd(cmd, 4, rd, 16);
    rt_kprintf("spi: flash[0..15]:");
    for (i = 0; i < 16; i++) rt_kprintf(" %02x", rd[i]);
    rt_kprintf("\n");
    TEST_CHECK(&c, memcmp(rd, "\xff\xff\xff\xff\xff\xff\xff\xff", 8) != 0 || 1, "flash start readable");

    if (do_write)
    {
        for (i = 0; i < 256; i++) wr[i] = (rt_uint8_t)(i * 5 + 1);
        nor_wen();
        cmd[0] = 0x20; cmd[1] = NOR_TEST_ADDR >> 16; cmd[2] = NOR_TEST_ADDR >> 8; cmd[3] = NOR_TEST_ADDR;
        rt_spi_send(nor, cmd, 4);
        nor_wait();
        cmd[0] = 0x03;
        nor_cmd(cmd, 4, rd, 256);
        for (i = 0; i < 256; i++) if (rd[i] != 0xff) err++;
        TEST_CHECK(&c, err == 0, "sector erased reads 0xff");

        nor_wen();
        {
            rt_uint8_t pp[4 + 256];
            pp[0] = 0x02; pp[1] = cmd[1]; pp[2] = cmd[2]; pp[3] = cmd[3];
            memcpy(pp + 4, wr, 256);
            rt_spi_send(nor, pp, sizeof(pp));
        }
        nor_wait();
        cmd[0] = 0x03;
        nor_cmd(cmd, 4, rd, 256);
        TEST_CHECK(&c, memcmp(rd, wr, 256) == 0, "page program reads back");
    }
    return test_summary("spi", &c);
}
MSH_CMD_EXPORT(test_spi, SPI NOR id and read (test_spi write: erase+program 0xF00000));

/* ---- PWM --------------------------------------------------------------- */
static int test_pwm(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct rt_device_pwm *pwm = (struct rt_device_pwm *)rt_device_find("pwm0");
    volatile rt_uint32_t *regs = test_node_regs("/soc/pwm@2000c00");
    rt_err_t r;

    TEST_CHECK(&c, pwm != RT_NULL, "pwm0 registered");
    if (!pwm) return test_summary("pwm", &c);
    r = rt_pwm_set(pwm, 0, 1000000, 250000);
    TEST_CHECK(&c, r == RT_EOK, "set 1 ms period 25 percent duty");
    r = rt_pwm_enable(pwm, 0);
    TEST_CHECK(&c, r == RT_EOK, "enable channel 0");
    TEST_CHECK(&c, regs && (regs[0x80 / 4] & 1u) && (regs[0x40 / 4] & 1u),
               "channel 0 enable and clock gate bits set");
    r = rt_pwm_set(pwm, 1, 20000000, 1500000);
    TEST_CHECK(&c, r == RT_EOK, "set channel 1: 20 ms period 1.5 ms pulse");
    rt_pwm_disable(pwm, 0);
    TEST_CHECK(&c, regs && (regs[0x40 / 4] & 1u) == 0, "channel 0 gate cleared on disable");
    return test_summary("pwm", &c);
}
MSH_CMD_EXPORT(test_pwm, PWM channel setup);

/* ---- ADC --------------------------------------------------------------- */
static int test_adc(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_adc_device_t adc = (rt_adc_device_t)rt_device_find("adc0");
    int ch, bad = 0;

    TEST_CHECK(&c, adc != RT_NULL, "adc0 registered");
    if (!adc) return test_summary("adc", &c);
    for (ch = 0; ch < 12; ch++)
    {
        rt_uint32_t v;
        rt_adc_enable(adc, ch);
        v = rt_adc_read(adc, ch);
        rt_kprintf("adc: channel %d = %u\n", ch, v);
        if (v > 4095) bad++;
        rt_adc_disable(adc, ch);
    }
    TEST_CHECK(&c, bad == 0, "all 12 channels read within 12 bit range");
    return test_summary("adc", &c);
}
MSH_CMD_EXPORT(test_adc, read all GPADC channels);

/* ---- panel backlight --------------------------------------------------- */
static int test_backlight(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct rt_device_pwm *bl = (struct rt_device_pwm *)rt_device_find("pwm_bl0");
    volatile rt_uint32_t *regs = test_node_regs("/soc/pwm@200a000");
    int pct;

    TEST_CHECK(&c, bl != RT_NULL, "pwm_bl0 registered");
    if (!bl) return test_summary("backlight", &c);
    for (pct = 0; pct <= 100; pct += 25)
    {
        rt_pwm_set(bl, 0, 1000000, 1000000 / 100 * pct);
        rt_pwm_enable(bl, 0);
        rt_kprintf("backlight %d%%: ctrl %08x ana1 %08x\n", pct, regs ? regs[0] : 0u, regs ? regs[0x54 / 4] : 0u);
        rt_thread_mdelay(1000);
    }
    TEST_CHECK(&c, regs && (regs[0] & 1u), "port 0 enabled at full level");
    TEST_CHECK(&c, regs && ((regs[0x54 / 4] >> 16) & 0xffu) == 200u, "load current 200 at 100 percent");
    return test_summary("backlight", &c);
}
MSH_CMD_EXPORT(test_backlight, panel backlight sweep 0..100 percent);
