/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO. Nothing is wired to the test pad, so the pull
 * resistors of the pad are used to make levels and edges.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "test.h"

#define TEST_PIN  (4 * 32 + 5)      /* PE5 */

static volatile int edges;
static void edge_cb(void *arg) { edges++; }

static int test_gpio(int argc, char **argv)
{
    struct test_ctx c = {0, 0};

    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT_PULLUP);
    rt_thread_mdelay(5);
    TEST_CHECK(&c, rt_pin_read(TEST_PIN) == 1, "input with pull-up reads 1");
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_thread_mdelay(5);
    TEST_CHECK(&c, rt_pin_read(TEST_PIN) == 0, "input with pull-down reads 0");

    rt_pin_mode(TEST_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(TEST_PIN, 1);
    TEST_CHECK(&c, rt_pin_read(TEST_PIN) == 1, "output high reads 1");
    rt_pin_write(TEST_PIN, 0);
    TEST_CHECK(&c, rt_pin_read(TEST_PIN) == 0, "output low reads 0");

    /* edges: the pad floats, the pull direction decides its level */
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT);
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_thread_mdelay(5);
    edges = 0;
    rt_pin_attach_irq(TEST_PIN, PIN_IRQ_MODE_RISING, edge_cb, RT_NULL);
    rt_pin_irq_enable(TEST_PIN, PIN_IRQ_ENABLE);
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT_PULLUP);
    rt_thread_mdelay(20);
    TEST_CHECK(&c, edges == 1, "rising edge interrupt counted once");
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_thread_mdelay(20);
    TEST_CHECK(&c, edges == 1, "falling edge ignored in rising mode");
    rt_pin_irq_enable(TEST_PIN, PIN_IRQ_DISABLE);

    rt_pin_attach_irq(TEST_PIN, PIN_IRQ_MODE_FALLING, edge_cb, RT_NULL);
    rt_pin_irq_enable(TEST_PIN, PIN_IRQ_ENABLE);
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT_PULLUP);
    rt_thread_mdelay(20);
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_thread_mdelay(20);
    TEST_CHECK(&c, edges == 2, "falling edge interrupt counted");
    rt_pin_irq_enable(TEST_PIN, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(TEST_PIN);
    rt_pin_mode(TEST_PIN, PIN_MODE_INPUT);
    return test_summary("gpio", &c);
}
MSH_CMD_EXPORT(test_gpio, GPIO levels pulls and interrupts on PE5);
