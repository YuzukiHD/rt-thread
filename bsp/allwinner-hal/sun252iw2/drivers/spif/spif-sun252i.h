/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI NOR flash controller (SPIF). Besides reading, programming and erasing
 * (also through the mtd_nor device "spif0") the driver can tune the sample point
 * of the read data (needed above a few ten MHz, where the return path of the clock
 * and data lines is longer than a clock period), keep the tuned sample point in a
 * flash partition, and map a part of the flash into the address space (XIP) so
 * that data can be read and code executed in place.
 */
#ifndef __SPIF_SUN252I_H__
#define __SPIF_SUN252I_H__

#include <rtthread.h>

/* sample modes tried by the tuning, and sample delay steps per mode */
#define SUN252I_SPIF_TUNE_MODES  3
#define SUN252I_SPIF_TUNE_DELAYS 64

struct sun252i_spif_info
{
    rt_uint32_t version;        /* controller version register */
    rt_uint8_t jedec_id[3];
    rt_uint32_t size;           /* flash capacity in bytes */
    rt_uint32_t frequency;      /* SCK in Hz in use */
    rt_bool_t quad;             /* four wire read/program in use */
    rt_bool_t dtr;              /* reads use double data rate (1-4-4 DTR) */
    rt_bool_t addr_4byte;
    rt_bool_t sample_tuned;     /* a sample point is applied (tuned or loaded) */
    rt_uint8_t sample_mode;
    rt_uint8_t sample_delay;
    rt_bool_t xip_active;
    rt_uint32_t xip_flash_offset;
    rt_uint32_t xip_length;
};

struct sun252i_spif_tune_result
{
    rt_uint32_t frequency;      /* the frequency the result is for (the last one tried on failure) */
    rt_uint8_t mode;
    rt_uint8_t delay;           /* middle of the widest passing window */
    rt_uint8_t window_start;
    rt_uint8_t window_len;
    rt_bool_t dtr;              /* the point is for DTR reads */
    /* bit n of ok[mode]: delay n reads the ID and the reference data correctly */
    rt_uint64_t ok[SUN252I_SPIF_TUNE_MODES];
    /* the same for the ID alone (one wire command), at the last frequency tried */
    rt_uint64_t ok_one_wire[SUN252I_SPIF_TUNE_MODES];
};

rt_err_t sun252i_spif_get_info(struct sun252i_spif_info *info);

rt_ssize_t sun252i_spif_read(rt_uint32_t offset, void *buf, rt_size_t len);
rt_ssize_t sun252i_spif_write(rt_uint32_t offset, const void *buf, rt_size_t len);
/* offset and len are multiples of 4 KiB */
rt_err_t sun252i_spif_erase(rt_uint32_t offset, rt_size_t len);

/*
 * Search the sample point: the reference is the first 4 KiB of the tune area read at 24 MHz
 * (it must not be constant), then every sample mode and delay is tried at the configured
 * frequency, lower ones when no window of at least 8 steps exists. The middle of the widest
 * window is applied. With DTR the DTR points come first, SDR when none exists.
 * Returns 0, -RT_EINVAL, -RT_EBUSY (XIP mapped), -RT_ENOSYS (no usable data) or -RT_EIO.
 */
rt_err_t sun252i_spif_tune(struct sun252i_spif_tune_result *res);
/* apply a sample point at the operating frequency */
rt_err_t sun252i_spif_set_sample(rt_uint8_t mode, rt_uint8_t delay);

/* the sample point in the params sector: -RT_ENOSYS without one, -RT_EEMPTY without a valid record */
rt_err_t sun252i_spif_params_save(void);
rt_err_t sun252i_spif_params_load(void);
rt_err_t sun252i_spif_params_erase(void);

/*
 * Map flash from flash_offset (4 KiB aligned) at the window; flash commands keep working
 * (the mapping is suspended around each one with interrupts masked), no code may run from the
 * window on another context meanwhile and the driver code must not live in it.
 */
rt_err_t sun252i_spif_xip_enable(rt_uint32_t flash_offset, rt_size_t length);
rt_err_t sun252i_spif_xip_disable(void);
const void *sun252i_spif_xip_window(void);

#endif
