/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-chip codec: a 440 Hz tone on the headphone/speaker output for 5 s; the
 * frames actually consumed by the DAC are counted against the 24 MHz timer, so
 * the rate is a measurement, not an echo of the setting. Listening is up to you.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <math.h>
#include "codec-sun252i.h"
#include "test.h"

static rt_uint32_t measured_pos;

static int play(struct test_ctx *c, rt_uint32_t rate, int seconds)
{
    rt_device_t dev = rt_device_find("audio0");
    struct rt_audio_caps caps;
    struct rt_audio_buf_info info;
    rt_uint32_t frames = 0, t0, t1, i, blocks = 0, total, cb0 = 0, cnt0 = 0;
    rt_int16_t *blk;
    double phase = 0, step = 2.0 * 3.14159265358979 * 440.0 / rate;
    char name[48];

    rt_snprintf(name, sizeof(name), "open at %u Hz", rate);
    TEST_CHECK(c, rt_device_open(dev, RT_DEVICE_OFLAG_WRONLY) == RT_EOK, name);

    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = rate;
    caps.udata.config.channels = 2;
    caps.udata.config.samplebits = 16;
    TEST_CHECK(c, rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps) == RT_EOK, "configure 16 bit stereo");
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type = AUDIO_MIXER_VOLUME;
    caps.udata.value = 50;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);

    info.block_size = 4800;     /* the driver's DMA block: 1200 stereo frames */
    blk = rt_malloc(info.block_size);
    total = rate * seconds / (info.block_size / 4);
    t0 = 0;
    for (i = 0; i < total; i++)
    {
        rt_uint32_t k;
        for (k = 0; k < info.block_size / 4; k++)
        {
            rt_int16_t v = (rt_int16_t)(sin(phase) * 12000.0);
            blk[2 * k] = blk[2 * k + 1] = v;
            phase += step;
            if (phase > 6.2831853) phase -= 6.2831853;
        }
        if (i == 20) { t0 = test_mtime(); cb0 = sun252i_codec_tx_blocks(); cnt0 = sun252i_codec_dac_count(); }          /* steady state: the writer is paced by the DAC */
        rt_device_write(dev, 0, blk, info.block_size);
        blocks++;
    }
    t1 = test_mtime();
    frames = (blocks - 21) * (info.block_size / 4);
    {
        rt_uint32_t us = (t1 - t0) / 24;
        rt_uint32_t measured = (rt_uint32_t)((rt_uint64_t)frames * 1000000u / us);
        rt_uint32_t hw = (rt_uint32_t)((rt_uint64_t)(sun252i_codec_tx_blocks() - cb0) * 1200u * 1000000u / us);
        rt_kprintf("audio: writer %u frames in %u us = %u Hz; DMA blocks %u => %u Hz (set %u)\n", frames, us, measured,
                   sun252i_codec_tx_blocks() - cb0, hw, rate);
        rt_kprintf("audio: DAC_CNT delta %u over the window\n", sun252i_codec_dac_count() - cnt0);
        measured = hw;
        
    }
    {
        /* the DMA read position over 10 ms: bytes / 4 = frames played */
        rt_uint32_t pa = sun252i_codec_tx_position(), ta = test_mtime(), pb, tb;
        while (test_mtime() - ta < 1920000u) ;
        pb = sun252i_codec_tx_position();
        tb = test_mtime();
        {
            rt_uint32_t d = (pb + 19200u - pa) % 19200u;
            rt_uint32_t fr = d / 4u;
            rt_uint32_t hz = (rt_uint32_t)((rt_uint64_t)fr * 24000000u / (tb - ta));
            rt_kprintf("audio: DMA position %u -> %u in %u ticks = %u frames = %u Hz\n", pa, pb, tb - ta, fr, hz);
            measured_pos = hz;
        }
    }
    TEST_CHECK(c, measured_pos > rate * 99 / 100 && measured_pos < rate * 101 / 100, "DMA position rate within 1 percent");
    rt_device_control(dev, AUDIO_CTL_STOP, RT_NULL);
    rt_device_close(dev);
    rt_free(blk);
    return 0;
}

static int test_audio(int argc, char **argv)
{
    struct test_ctx c = {0, 0};

    TEST_CHECK(&c, rt_device_find("audio0") != RT_NULL, "audio0 registered");
    if (rt_device_find("audio0") == RT_NULL) return test_summary("audio", &c);
    play(&c, 48000, 5);
    play(&c, 44100, 3);
    return test_summary("audio", &c);
}
MSH_CMD_EXPORT(test_audio, codec 440 Hz tone with measured sample rate);

/*
 * Microphone: three seconds of capture. The pass criteria are what the board
 * can prove by itself: the ADC counts samples at 48 kHz and the receive DMA
 * delivers its blocks. The sample levels are printed; with nothing connected
 * to the microphone input they are not required to move.
 */
static int test_mic(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_device_t dev = rt_device_find("audio0");
    struct rt_audio_caps caps;
    rt_uint8_t *buf = rt_malloc(4096);
    rt_uint32_t n = 0, t0, t1, cnt0, cnt1, cb0;
    rt_int32_t mn = 32767, mx = -32768;

    TEST_CHECK(&c, dev != RT_NULL && buf != RT_NULL, "audio0 and buffer");
    if (!dev || !buf) return test_summary("mic", &c);
    TEST_CHECK(&c, rt_device_open(dev, RT_DEVICE_OFLAG_RDONLY) == RT_EOK, "open for recording");
    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = 48000;
    caps.udata.config.channels = 1;
    caps.udata.config.samplebits = 16;
    TEST_CHECK(&c, rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps) == RT_EOK, "configure 48 kHz mono");
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type = AUDIO_MIXER_MIC;
    caps.udata.value = 1;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);
    rt_device_control(dev, AUDIO_CTL_START, RT_NULL);
    rt_thread_mdelay(200);
    t0 = test_mtime();
    cnt0 = *(volatile rt_uint32_t *)0x02030044u;
    cb0 = sun252i_codec_rx_blocks();
    while (test_mtime() - t0 < 24000000u * 3)
    {
        rt_size_t r = rt_device_read(dev, 0, buf, 2048);
        rt_uint32_t i;
        rt_int16_t *s = (rt_int16_t *)buf;

        for (i = 0; i < r / 2; i++, n++)
        {
            if (s[i] < mn) mn = s[i];
            if (s[i] > mx) mx = s[i];
        }
        if (r == 0) rt_thread_mdelay(5);
    }
    t1 = test_mtime();
    cnt1 = *(volatile rt_uint32_t *)0x02030044u;
    cb0 = sun252i_codec_rx_blocks() - cb0;
    rt_device_control(dev, AUDIO_CTL_STOP, RT_NULL);
    rt_device_close(dev);
    {
        rt_uint32_t hz = (rt_uint32_t)((rt_uint64_t)(cnt1 - cnt0) * 24000000u / (t1 - t0));
        rt_kprintf("mic: ADC counted %u samples = %u Hz, %u DMA blocks, read %u samples, min %d max %d\n",
                   cnt1 - cnt0, hz, cb0, n, mn, mx);
        TEST_CHECK(&c, hz > 47000 && hz < 49000, "ADC sample rate within 2 percent of 48 kHz");
        TEST_CHECK(&c, cb0 >= 50, "receive DMA delivers its blocks (about 20 per second)");
    }
    if (mx == mn)
        rt_kprintf("[INFO] samples are constant (%d): no signal on the microphone input\n", mn);
    rt_free(buf);
    return test_summary("mic", &c);
}
MSH_CMD_EXPORT(test_mic, microphone capture: ADC rate and DMA blocks);
