/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * General purpose ADC of the sun252i SoC: twelve 12 bit channels, converted one at a time
 * with the controller running continuously.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>

#define GP_SR_REG       0x00u
#define GP_CTRL_REG     0x04u
#define GP_CS_EN_REG    0x08u
#define GP_DATA_INTS_REG 0x38u
#define GP_CH0_DATA_REG 0x80u

#define GP_CTRL_VCM_BUF_EN 0x00000001u
#define GP_CTRL_CALIBRATION 0x00020000u
#define GP_CTRL_MODE_MASK   0x000c0000u
#define GP_CTRL_CONTINUOUS  0x00080000u
#define GP_CTRL_ENABLE      0x00010000u
#define GP_DATA_MASK        0x00000fffu

#define ADC_CHANNELS    12u
#define ADC_CLOCK_HZ    24000000u
#define ADC_SAMPLE_RATE 1000000u
#define ADC_TIMEOUT_US  10000u


struct sun_adc
{
    struct rt_adc_device adc;
    rt_ubase_t base;
};

#define raw_to_adc(dev) rt_container_of(dev, struct sun_adc, adc)

static rt_uint32_t adc_read(struct sun_adc *a, rt_uint32_t off)
{
    return HWREG32(a->base + off);
}

static void adc_write(struct sun_adc *a, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32(a->base + off) = val;
}

static rt_err_t adc_enabled(rt_adc_device_t device, rt_int8_t channel, rt_bool_t enabled)
{
    RT_UNUSED(device);
    RT_UNUSED(enabled);
    if (channel < 0 || channel >= ADC_CHANNELS)
    {
        return -RT_EINVAL;
    }

    /* the controller converts the selected channel continuously */
    return RT_EOK;
}

static rt_err_t adc_convert(rt_adc_device_t device, rt_int8_t channel, rt_uint32_t *value)
{
    struct sun_adc *a = raw_to_adc(device);
    rt_uint32_t bit, n;

    if (channel < 0 || channel >= ADC_CHANNELS)
    {
        return -RT_EINVAL;
    }
    bit = 1u << (rt_uint32_t)channel;

    adc_write(a, GP_CS_EN_REG, bit);
    adc_write(a, GP_DATA_INTS_REG, bit);
    for (n = 0u; n < ADC_TIMEOUT_US; n++)
    {
        if (adc_read(a, GP_DATA_INTS_REG) & bit)
        {
            *value = adc_read(a, GP_CH0_DATA_REG + (rt_uint32_t)channel * 4u) & GP_DATA_MASK;
            /* the pending word is write-one-to-clear */
            adc_write(a, GP_DATA_INTS_REG, bit);
            return RT_EOK;
        }
        rt_hw_us_delay(1u);
    }

    return -RT_ETIMEOUT;
}

static const struct rt_adc_ops adc_ops =
{
    adc_enabled,
    adc_convert,
    RT_NULL,
    RT_NULL,
};

static rt_err_t adc_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct sun_adc *a = rt_calloc(1, sizeof(*a));
    struct rt_clk *clk;
    struct rt_reset_control *rst;
    rt_uint32_t reg;
    rt_err_t err;

    if (!a)
    {
        return -RT_ENOMEM;
    }

    a->base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    clk = rt_clk_get_by_index(dev, 0);
    rst = rt_reset_control_get_by_index(dev, 0);
    if (!a->base || rt_is_err_or_null(clk) || rt_is_err_or_null(rst))
    {
        rt_free(a);

        return -RT_ERROR;
    }
    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(clk);

    reg = adc_read(a, GP_SR_REG);
    reg = (reg & 0x0000ffffu) | ((ADC_CLOCK_HZ / ADC_SAMPLE_RATE - 1u) << 16);
    adc_write(a, GP_SR_REG, reg);

    /* continuous conversion, calibration and the input buffer */
    reg = adc_read(a, GP_CTRL_REG) & ~GP_CTRL_MODE_MASK;
    reg |= GP_CTRL_CONTINUOUS | GP_CTRL_CALIBRATION | GP_CTRL_VCM_BUF_EN | GP_CTRL_ENABLE;
    adc_write(a, GP_CTRL_REG, reg);

    a->adc.ops = &adc_ops;
    a->adc.parent.ofw_node = dev->ofw_node;
    dev->user_data = a;
    rt_dm_dev_set_name_auto(&a->adc.parent, "adc");
    err = rt_hw_adc_register(&a->adc, rt_dm_dev_get_name(&a->adc.parent), &adc_ops, a);
    if (err)
    {
        rt_free(a);
    }

    return err;
}

static const struct rt_ofw_node_id adc_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-gpadc" },
    { /* sentinel */ }
};

static struct rt_platform_driver adc_driver =
{
    .name = "adc-sun252i",
    .ids = adc_ofw_ids,
    .probe = adc_probe,
};
RT_PLATFORM_DRIVER_EXPORT(adc_driver);
