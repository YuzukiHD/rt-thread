/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <string.h>
#include "dma-sun252i.h"
#include "codec-sun252i.h"

/* ---- registers ------------------------------------------------------------------------- */


/* digital */
#define DAC_FIFO_CTL        0x10
#define DAC_FIFO_STA        0x14
#define DAC_CNT             0x24
#define ADC_FIFO_CTL        0x30
#define ADC_FIFO_STA        0x38
#define ADC_CNT             0x44
#define ADC_DIG_CTL         0x50
#define DAC_FS_SHIFT        29
#define DAC_FIFO_MODE_SHIFT 24
#define DAC_MONO_EN         (1u << 6)
#define DAC_TX_SAMPLE_24    (1u << 5)
#define DAC_DRQ_EN          (1u << 4)
#define DAC_FIFO_FLUSH      (1u << 0)
#define DAC_STA_CLEAR       ((1u << 3) | (1u << 2) | (1u << 1))
#define ADC_FS_SHIFT        29
#define ADC_EN              (1u << 28)
#define ADC_RX_FIFO_16      (1u << 24)
#define ADC_RX_SAMPLE_24    (1u << 16)
#define ADC_DRQ_EN          (1u << 3)
#define ADC_FIFO_FLUSH      (1u << 0)
#define ADC_STA_CLEAR       ((1u << 3) | (1u << 1))
#define ADC1_CHANNEL_EN     (1u << 0)

/* analog (same address space) */
#define A_DAC_DPC           0x00
#define A_DAC_VOL_CTL       0x04
#define A_ADC_VOL_CTL       0x34
#define A_ADC1_AN           0x300
#define A_DAC_AN            0x310
#define A_RAMP              0x31c
#define A_HP2               0x340
#define DAC_DIG_EN          (1u << 31)
#define DAC_DVOL_SHIFT      12
#define DAC_DVOL_MASK       (0x3fu << DAC_DVOL_SHIFT)
#define DAC_VOL_L_SHIFT     8
#define DAC_VOL_R_SHIFT     0
#define DAC_VOL_SEL         (1u << 16)
#define ADC1_EN             (1u << 31)
#define MIC1_PGA_EN         (1u << 30)
#define LINEINLEN           (1u << 23)
#define ADC1_PGA_GAIN_SHIFT 8
#define DACL_EN             (1u << 15)
#define DACR_EN             (1u << 14)
#define LMUTE               (1u << 12)
#define RMUTE               (1u << 10)
#define RMC_EN              (1u << 1)
#define RK_OPT_EN           (1u << 21)
#define HPFB_BUF_EN         (1u << 31)
#define HP_GAIN_SHIFT       28
#define HP_GAIN_MASK        (0x7u << HP_GAIN_SHIFT)
#define HP_DRVEN            (1u << 21)
#define RSWITCH             (1u << 19)
#define HPFB_IN_EN          (1u << 17)
#define RAMP_OUT_EN         (1u << 15)


/* PE10: speaker amplifier enable */

#define TX_BLOCKS           4
#define TX_BLOCK_FRAMES     1200            /* stereo 16 bit @ 48 kHz: 25 ms */
#define RX_BLOCKS           4
#define RX_BLOCK_FRAMES     1200

#define AUDIO_IRQ_NONE      0

static rt_ubase_t codec_base;
static rt_base_t speaker_pin = -1;
static struct rt_clk *dac_clk, *adc_clk;
static struct rt_dma_chan *tx_chan, *rx_chan;

static rt_uint32_t codec_rd(rt_uint32_t off)
{
    return HWREG32(codec_base + off);
}

static void codec_wr(rt_uint32_t off, rt_uint32_t v)
{
    HWREG32(codec_base + off) = v;
}

static void codec_upd(rt_uint32_t off, rt_uint32_t mask, rt_uint32_t v)
{
    codec_wr(off, (codec_rd(off) & ~mask) | (v & mask));
}

/* ---- analog front end ------------------------------------------------------------------ */

static void codec_upd_a(rt_uint32_t off, rt_uint32_t mask, rt_uint32_t v)
{
    codec_upd(off, mask, v);
}

static rt_bool_t output_on;
static rt_bool_t muted[2] = {RT_TRUE, RT_TRUE};

static void apply_mute(void)
{
    rt_uint32_t on = 0u;

    if (output_on)
    {
        on |= muted[0] ? 0u : LMUTE;
        on |= muted[1] ? 0u : RMUTE;
    }
    codec_upd_a(A_DAC_AN, LMUTE | RMUTE, on);
}

static void sunxi_audio_speaker(rt_bool_t on)
{
    if (speaker_pin < 0)
        return;
    rt_pin_mode(speaker_pin, PIN_MODE_OUTPUT);
    rt_pin_write(speaker_pin, on ? PIN_HIGH : PIN_LOW);
}

static void analog_start_output(void)
{
    if (!output_on)
    {
        output_on = RT_TRUE;
        apply_mute();
        codec_upd_a(A_DAC_DPC, DAC_DIG_EN, DAC_DIG_EN);
        codec_upd_a(A_DAC_AN, DACL_EN | DACR_EN, DACL_EN | DACR_EN);
        codec_upd_a(A_HP2, HPFB_BUF_EN | HPFB_IN_EN, HPFB_BUF_EN | HPFB_IN_EN);
        codec_upd_a(A_HP2, RAMP_OUT_EN | RSWITCH, RAMP_OUT_EN | RSWITCH);
        codec_upd_a(A_HP2, HP_DRVEN, HP_DRVEN);
        sunxi_audio_speaker(RT_TRUE);
        rt_thread_mdelay(20);
    }
}

static void analog_stop_output(void)
{
    if (output_on)
    {
        sunxi_audio_speaker(RT_FALSE);
        codec_upd_a(A_HP2, HP_DRVEN, 0u);
        output_on = RT_FALSE;
        apply_mute();
        codec_upd_a(A_HP2, RAMP_OUT_EN | RSWITCH, 0u);
        codec_upd_a(A_HP2, HPFB_BUF_EN | HPFB_IN_EN, 0u);
        codec_upd_a(A_DAC_DPC, DAC_DIG_EN, 0u);
        codec_upd_a(A_DAC_AN, DACL_EN | DACR_EN, 0u);
    }
}

/* the family of the audio PLL a rate belongs to and the module clock it needs */
#define FAMILY_48K  0
#define FAMILY_44K1 1

static rt_int32_t audio_family(rt_uint32_t rate)
{
    if (rate && rate % 8000u == 0u)
        return FAMILY_48K;
    if (rate && rate % 11025u == 0u)
        return FAMILY_44K1;
    return -1;
}

static rt_uint32_t audio_base_mclk(rt_int32_t family)
{
    return family == FAMILY_48K ? 24576000u : 22579200u;
}

/* ---- RT-Thread audio device ------------------------------------------------------------ */

struct sunxi_audio
{
    struct rt_audio_device dev;
    struct rt_audio_buf_info tx_info;
    rt_uint8_t tx_buf[TX_BLOCKS * TX_BLOCK_FRAMES * 4] __attribute__((aligned(64)));
    rt_uint8_t rx_buf[RX_BLOCKS * RX_BLOCK_FRAMES * 4] __attribute__((aligned(64)));
    rt_uint32_t tx_rate, rx_rate;
    rt_uint8_t tx_ch, rx_ch, tx_bits, rx_bits;
    rt_bool_t tx_on, rx_on;
    rt_uint32_t tx_frames, rx_frames;
};

static struct sunxi_audio audio0;
static rt_uint32_t tx_period_ns;

static rt_err_t audio_getcaps(struct rt_audio_device *dev, struct rt_audio_caps *caps);
static rt_err_t audio_configure(struct rt_audio_device *dev, struct rt_audio_caps *caps);
static rt_err_t audio_init(struct rt_audio_device *dev);
static rt_err_t audio_start(struct rt_audio_device *dev, int stream);
static rt_err_t audio_stop(struct rt_audio_device *dev, int stream);
static rt_ssize_t audio_transmit(struct rt_audio_device *dev, const void *wb, void *rb, rt_size_t size);
static void audio_buffer_info(struct rt_audio_device *dev, struct rt_audio_buf_info *info);

/* rate code of the normalized 48 kHz family rate */
static rt_int32_t dac_rate_code(rt_uint32_t rate)
{
    switch (rate)
    {
    case 48000: return 0;
    case 32000: return 1;
    case 24000: return 2;
    case 16000: return 3;
    case 12000: return 4;
    case 8000:  return 5;
    case 192000: return 6;
    case 96000: return 7;
    default: return -1;
    }
}

static rt_err_t audio_configure(struct rt_audio_device *dev, struct rt_audio_caps *caps)
{
    struct sunxi_audio *a = (struct sunxi_audio *)dev;

    if (caps->main_type == AUDIO_TYPE_MIXER)
    {
        switch (caps->sub_type)
        {
        case AUDIO_MIXER_VOLUME:
        {
            rt_uint32_t v = (rt_uint32_t)caps->udata.value * 63u / AUDIO_VOLUME_MAX;

            /* digital volume plus the headphone driver gain */
            codec_upd_a(A_DAC_VOL_CTL, DAC_DVOL_MASK, v << DAC_DVOL_SHIFT);
            codec_upd_a(A_HP2, HP_GAIN_MASK, (2u << HP_GAIN_SHIFT) & HP_GAIN_MASK);
            return RT_EOK;
        }
        case AUDIO_MIXER_MUTE:
            muted[0] = muted[1] = caps->udata.value ? RT_TRUE : RT_FALSE;
            apply_mute();
            return RT_EOK;
        case AUDIO_MIXER_MIC:
            if (caps->udata.value)
            {
                /* analog ADC and the microphone amplifier, gain 15 (step 0..15) */
                codec_upd_a(A_ADC1_AN, ADC1_EN | MIC1_PGA_EN | (0xfu << ADC1_PGA_GAIN_SHIFT),
                            ADC1_EN | MIC1_PGA_EN | (15u << ADC1_PGA_GAIN_SHIFT));
                codec_upd_a(A_ADC_VOL_CTL, 0xffu, 160u);
            }
            else
                codec_upd_a(A_ADC1_AN, ADC1_EN | MIC1_PGA_EN, 0u);
            return RT_EOK;
        default:
            return -1;
        }
    }
    if (caps->main_type != AUDIO_TYPE_OUTPUT && caps->main_type != AUDIO_TYPE_INPUT)
        return -1;
    {
        rt_bool_t tx = caps->main_type == AUDIO_TYPE_OUTPUT;
        struct rt_audio_configure *c = &caps->udata.config;
        rt_int32_t fam = audio_family(c->samplerate);
        rt_uint32_t mclk = audio_base_mclk(fam);
        rt_uint32_t period_ns;

        if (fam < 0 || c->channels < 1u || c->channels > 2u ||
            (c->samplebits != 16u && c->samplebits != 24u))
            return -1;
        /* frame period of one block */
        period_ns = (1000000000u / c->samplerate) * TX_BLOCK_FRAMES / TX_BLOCKS + 1u;
        tx_period_ns = period_ns;

        if (tx)
        {
            rt_int32_t code = dac_rate_code(fam == FAMILY_44K1 ?
                (rt_uint32_t)(((rt_uint64_t)c->samplerate * 48000u + 22050u) / 44100u) :
                c->samplerate);

            if (code < 0)
                return -1;
            a->tx_rate = c->samplerate;
            a->tx_ch = c->channels;
            a->tx_bits = c->samplebits;
            a->tx_frames = TX_BLOCK_FRAMES;
            (void)code;
        }
        else
        {
            if (fam != FAMILY_48K || c->samplerate > 48000u)
                return -1;
            a->rx_rate = c->samplerate;
            a->rx_ch = 1;
            a->rx_bits = 16;
            a->rx_frames = RX_BLOCK_FRAMES;
        }
        (void)mclk;
        return RT_EOK;
    }
}

static rt_err_t audio_getcaps(struct rt_audio_device *dev, struct rt_audio_caps *caps)
{
    (void)dev;
    if (caps->main_type == AUDIO_TYPE_QUERY)
    {
        caps->udata.mask = AUDIO_TYPE_OUTPUT | AUDIO_TYPE_INPUT | AUDIO_TYPE_MIXER;
        return RT_EOK;
    }
    if (caps->main_type == AUDIO_TYPE_INPUT)
    {
        caps->udata.config.channels = 1;
        caps->udata.config.samplebits = 16;
        caps->udata.config.samplerate = 48000;
        return RT_EOK;
    }
    if (caps->main_type == AUDIO_TYPE_OUTPUT)
    {
        caps->udata.config.channels = 2;
        caps->udata.config.samplebits = 16;
        caps->udata.config.samplerate = 48000;
        return RT_EOK;
    }
    return -1;
}

static rt_err_t audio_init(struct rt_audio_device *dev)
{
    struct sunxi_audio *a = (struct sunxi_audio *)dev;

    (void)a;
    /* volume control on, drop the first samples of a capture while the input settles */
    codec_upd(ADC_DIG_CTL, 1u << 16, 1u << 16);
    codec_upd(0x04, 1u << 16, 1u << 16);
    codec_upd(ADC_FIFO_CTL, (1u << 25) | (3u << 26), (1u << 25) | (2u << 26));
    memset(a->tx_buf, 0, sizeof(a->tx_buf));
    return RT_EOK;
}

static volatile rt_uint32_t tx_blocks;
static volatile rt_uint32_t rx_blocks;

static void audio_dma_tx_cb(struct rt_dma_chan *chan, rt_size_t size)
{
    if (size)
    {
        tx_blocks++;
        rt_audio_tx_complete(&audio0.dev);
    }
}

static void audio_dma_rx_cb(struct rt_dma_chan *chan, rt_size_t size)
{
    static rt_uint32_t block;

    if (size)
    {
        /* the DMA wrote around the cache: drop the stale lines before the data is used */
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, audio0.rx_buf + block * (RX_BLOCK_FRAMES * 4), RX_BLOCK_FRAMES * 4);
        rt_audio_rx_done(&audio0.dev, audio0.rx_buf + block * (RX_BLOCK_FRAMES * 4), RX_BLOCK_FRAMES * 4);
        block = (block + 1u) % RX_BLOCKS;
        rx_blocks++;
    }
}

/* a ring of @blocks blocks between the memory and a FIFO of the codec */
static rt_err_t ring_start(struct rt_dma_chan *chan, enum rt_dma_transfer_direction dir, rt_ubase_t fifo,
                           rt_uint8_t *buf, rt_size_t block_len, rt_uint32_t blocks, rt_uint32_t width,
                           void (*cb)(struct rt_dma_chan *, rt_size_t))
{
    struct rt_dma_slave_config conf = { .direction = dir };
    struct rt_dma_slave_transfer t = { 0 };
    rt_err_t err;

    conf.src_addr_width = conf.dst_addr_width = (enum rt_dma_slave_buswidth)width;
    conf.src_maxburst = conf.dst_maxburst = 4;
    if (dir == RT_DMA_MEM_TO_DEV)
    {
        conf.dst_addr = fifo;
        t.src_addr = (rt_ubase_t)buf;
    }
    else
    {
        conf.src_addr = fifo;
        t.dst_addr = (rt_ubase_t)buf;
    }
    t.buffer_len = block_len * blocks;
    t.period_len = block_len;
    chan->callback = cb;
    if ((err = rt_dma_chan_config(chan, &conf)))
        return err;
    if ((err = rt_dma_prep_cyclic(chan, &t)))
        return err;

    return rt_dma_chan_start(chan);
}

static rt_err_t audio_start(struct rt_audio_device *dev, int stream)
{
    struct sunxi_audio *a = (struct sunxi_audio *)dev;

    if (stream == AUDIO_STREAM_REPLAY)
    {
        rt_int32_t fam = audio_family(a->tx_rate ? a->tx_rate : 48000u);
        rt_uint32_t bytes = a->tx_bits == 24u ? 3u : 2u;
        rt_uint32_t frame = bytes * (a->tx_ch ? a->tx_ch : 2u);
        rt_uint32_t block_len = a->tx_frames * frame;
        rt_uint32_t v;

        if (rt_clk_set_rate(dac_clk, audio_base_mclk(fam)) != RT_EOK)
            return -1;
        rt_clk_prepare_enable(dac_clk);
        codec_upd(DAC_FIFO_CTL, DAC_DRQ_EN, 0u);
        codec_upd(DAC_FIFO_CTL, DAC_FIFO_FLUSH, DAC_FIFO_FLUSH);
        codec_wr(DAC_FIFO_STA, DAC_STA_CLEAR);
        codec_wr(DAC_CNT, 0u);
        analog_start_output();

        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, a->tx_buf, sizeof(a->tx_buf));
        /* one DMA beat per sample, 24 bit samples sit in words */
        if (ring_start(tx_chan, RT_DMA_MEM_TO_DEV, codec_base + 0x20u /* DAC_TXDATA */, a->tx_buf, block_len,
                       TX_BLOCKS, bytes == 3u ? 4u : bytes, audio_dma_tx_cb) != RT_EOK)
        {
            analog_stop_output();
            rt_clk_disable_unprepare(dac_clk);
            return -1;
        }
        v = 0u; /* 48 kHz normalized */
        v |= a->tx_bits == 24u ? DAC_TX_SAMPLE_24 : (3u << DAC_FIFO_MODE_SHIFT);
        if (a->tx_ch == 1u)
            v |= DAC_MONO_EN;
        codec_upd(DAC_FIFO_CTL, (7u << DAC_FS_SHIFT) | (3u << DAC_FIFO_MODE_SHIFT) | DAC_TX_SAMPLE_24 | DAC_MONO_EN, v);
        codec_upd(DAC_FIFO_CTL, DAC_DRQ_EN, DAC_DRQ_EN);
        a->tx_on = RT_TRUE;
    }
    else
    {
        if (rt_clk_set_rate(adc_clk, audio_base_mclk(FAMILY_48K)) != RT_EOK)
            return -1;
        rt_clk_prepare_enable(adc_clk);
        codec_upd(ADC_DIG_CTL, ADC1_CHANNEL_EN, ADC1_CHANNEL_EN);
        codec_upd(ADC_FIFO_CTL, ADC_FIFO_FLUSH | ADC_EN, ADC_FIFO_FLUSH | ADC_EN);
        codec_wr(ADC_FIFO_STA, ADC_STA_CLEAR);
        codec_wr(ADC_CNT, 0u);

        if (ring_start(rx_chan, RT_DMA_DEV_TO_MEM, codec_base + 0x40u /* ADC_RXDATA */, a->rx_buf,
                       RX_BLOCK_FRAMES * 4, RX_BLOCKS, 2u, audio_dma_rx_cb) != RT_EOK)
        {
            rt_clk_disable_unprepare(adc_clk);
            return -1;
        }
        codec_upd(ADC_FIFO_CTL, ADC_DRQ_EN, ADC_DRQ_EN);
        a->rx_on = RT_TRUE;
    }
    return RT_EOK;
}

static rt_err_t audio_stop(struct rt_audio_device *dev, int stream)
{
    struct sunxi_audio *a = (struct sunxi_audio *)dev;

    if (stream == AUDIO_STREAM_REPLAY)
    {
        rt_dma_chan_stop(tx_chan);
        codec_upd(DAC_FIFO_CTL, DAC_DRQ_EN, 0u);
        analog_stop_output();
        rt_clk_disable_unprepare(dac_clk);
        a->tx_on = RT_FALSE;
    }
    else
    {
        rt_dma_chan_stop(rx_chan);
        codec_upd(ADC_FIFO_CTL, ADC_DRQ_EN | ADC_EN, 0u);
        codec_upd(ADC_DIG_CTL, ADC1_CHANNEL_EN, 0u);
        rt_clk_disable_unprepare(adc_clk);
        a->rx_on = RT_FALSE;
    }
    return RT_EOK;
}

static rt_ssize_t audio_transmit(struct rt_audio_device *dev, const void *wb, void *rb, rt_size_t size)
{
    struct sunxi_audio *a = (struct sunxi_audio *)dev;
    static rt_uint32_t pos;

    (void)rb;
    if (size > sizeof(a->tx_buf))
        return 0;
    /* the framework hands out blocks of the buffer info: copy into the DMA ring */
    if (pos + size > sizeof(a->tx_buf))
        pos = 0;
    memcpy(a->tx_buf + pos, wb, size);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, a->tx_buf + pos, size);
    pos = (pos + (rt_uint32_t)size) % (rt_uint32_t)sizeof(a->tx_buf);
    return size;
}

static void audio_buffer_info(struct rt_audio_device *dev, struct rt_audio_buf_info *info)
{
    struct sunxi_audio *a = (struct sunxi_audio *)dev;

    info->buffer = a->tx_buf;
    info->block_size = TX_BLOCK_FRAMES * 4;
    info->block_count = TX_BLOCKS;
    info->total_size = sizeof(a->tx_buf);
}

static struct rt_audio_ops audio_ops =
{
    audio_getcaps,
    audio_configure,
    audio_init,
    audio_start,
    audio_stop,
    audio_transmit,
    audio_buffer_info,
};

rt_uint32_t sun252i_codec_rx_blocks(void)
{
    return rx_blocks;
}

rt_uint32_t sun252i_codec_tx_blocks(void)
{
    return tx_blocks;
}

rt_uint32_t sun252i_codec_dac_count(void)
{
    return codec_rd(DAC_CNT);
}

rt_size_t sun252i_codec_tx_position(void)
{
    return sun252i_dma_position(tx_chan);
}

static rt_err_t codec_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct rt_clk *bus;
    struct rt_reset_control *rst;
    rt_uint8_t mode, value;

    codec_base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    bus = rt_clk_get_by_name(dev, "bus");
    dac_clk = rt_clk_get_by_name(dev, "dac");
    adc_clk = rt_clk_get_by_name(dev, "adc");
    rst = rt_reset_control_get_by_index(dev, 0);
    tx_chan = rt_dma_chan_request(dev, "tx");
    rx_chan = rt_dma_chan_request(dev, "rx");
    if (!codec_base || rt_is_err_or_null(bus) || rt_is_err_or_null(dac_clk) || rt_is_err_or_null(adc_clk) ||
        rt_is_err_or_null(rst) || rt_is_err_or_null(tx_chan) || rt_is_err_or_null(rx_chan))
    {
        return -RT_ERROR;
    }

    rt_reset_control_assert(rst);
    rt_clk_prepare_enable(bus);
    rt_reset_control_deassert(rst);

    speaker_pin = rt_ofw_get_named_pin(dev->ofw_node, "speaker", 0, &mode, &value);

    audio0.dev.ops = &audio_ops;
    audio0.dev.parent.ofw_node = dev->ofw_node;
    dev->user_data = &audio0;

    return rt_audio_register(&audio0.dev, "audio0", RT_DEVICE_FLAG_RDWR, RT_NULL);
}

static const struct rt_ofw_node_id codec_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-codec" },
    { /* sentinel */ }
};

static struct rt_platform_driver codec_driver =
{
    .name = "codec-sun252i",
    .ids = codec_ofw_ids,
    .probe = codec_probe,
};
RT_PLATFORM_DRIVER_EXPORT(codec_driver);
