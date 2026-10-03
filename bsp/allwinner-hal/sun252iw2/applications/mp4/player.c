/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MP4 player: the H.264 track goes through the video engine onto the video plane of the display,
 * the AAC track is decoded in software and played by the audio codec. The sound is the master
 * clock (the frames the DAC has taken): a picture waits until the sound has reached its time
 * stamp, a picture that is too late is dropped.
 *
 *   mp4_play <file>     play a file of the mounted SD card (the card is mounted on first use)
 *   mp4_stop            stop the playback
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <dfs_fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>

#include "mp4.h"
#include "vdec.h"
#include "drv_display.h"
#include "codec-sun252i.h"
#include "pvmp4audiodecoder_api.h"

#define AAC_FRAMES      1024
#define PCM_BYTES       (AAC_FRAMES * 2 * sizeof(rt_int16_t))
/* the codec takes blocks of 1200 stereo frames */
#define BLOCK_BYTES     4800u
/* pictures later than this behind the sound are not shown */
#define DROP_US         (2 * 33367)
/* pictures are put up this much before their time, the display waits for a refresh */
#define LEAD_US         8000
#define REPORT_MS       5000

/*
 * A file with a read window: the samples of a track are small and spread over the file, a seek in
 * FAT walks the cluster chain, so a window is read at a time and the samples are cut out of it.
 */
struct reader
{
    int fd;
    rt_uint8_t *win;
    size_t win_size;
    rt_uint64_t win_off;
    size_t win_len;
};

static struct mp4 mp4;
static struct reader vfile, afile;
static volatile rt_bool_t audio_running, audio_done, stop_req, playing;
static volatile rt_uint32_t audio_underruns;
static rt_uint32_t audio_rate;

static int file_read(void *ctx, rt_uint64_t off, void *buf, size_t len)
{
    struct reader *r = ctx;
    ssize_t n;

    if (len > r->win_size)
    {
        if (lseek(r->fd, (off_t)off, SEEK_SET) < 0)
            return -EIO;
        n = read(r->fd, buf, len);
        return n == (ssize_t)len ? 0 : -EIO;
    }
    if (off < r->win_off || off + len > r->win_off + r->win_len)
    {
        if (lseek(r->fd, (off_t)off, SEEK_SET) < 0)
            return -EIO;
        n = read(r->fd, r->win, r->win_size);
        if (n < (ssize_t)len)
        {
            r->win_len = 0;
            return -EIO;
        }
        r->win_off = off;
        r->win_len = n;
    }
    memcpy(buf, r->win + (off - r->win_off), len);

    return 0;
}

/* ---- file system ----------------------------------------------------------------------- */

static int mount_card(void)
{
    static const char *const names[] = { "sd0p1", "sd0" };
    struct stat st;
    unsigned int i;

    if (stat("/", &st) == 0 && dfs_filesystem_get_mounted_path(rt_device_find("sd0p1")) != RT_NULL)
        return 0;
    for (i = 0; i < RT_ARRAY_SIZE(names); i++)
    {
        if (rt_device_find(names[i]) && dfs_mount(names[i], "/", "elm", 0, RT_NULL) == 0)
        {
            rt_kprintf("mp4: %s mounted on /\n", names[i]);
            return 0;
        }
    }

    return stat("/", &st) == 0 ? 0 : -1;
}

/* ---- audio ----------------------------------------------------------------------------- */

/* position of the sound in microseconds, -1 before it started */
static rt_int64_t audio_clock_us(void)
{
    if (!audio_running)
        return -1;

    /* the DAC counts both channels */
    return (rt_int64_t)(sun252i_codec_dac_count() / 2u) * 1000000 / audio_rate;
}

static void audio_main(void *arg)
{
    const struct mp4_track *t = &mp4.audio;
    rt_device_t dev = rt_device_find("audio0");
    struct rt_audio_caps caps;
    tPVMP4AudioDecoderExternal ext;
    void *dec = rt_malloc(PVMP4AudioDecoderGetMemRequirements());
    rt_int16_t *pcm = rt_malloc(PCM_BYTES * 2);
    rt_uint8_t *fifo = rt_malloc(BLOCK_BYTES + PCM_BYTES);
    rt_uint8_t *buf = rt_malloc(t->max_sample);
    size_t fifo_len = 0;
    struct mp4_iter it;
    struct mp4_sample s;
    rt_uint32_t skip = t->media_start / AAC_FRAMES;
    int ret;

    memset(&ext, 0, sizeof(ext));
    ext.desiredChannels = 2;
    ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
    ext.aacPlusEnabled = false;
    ext.pOutputBuffer = pcm;
    ext.pOutputBuffer_plus = pcm + AAC_FRAMES * 2;
    if (!dev || !dec || !pcm || !fifo || !buf || PVMP4AudioDecoderInitLibrary(&ext, dec) != 0)
    {
        rt_kprintf("mp4: cannot start the AAC decoder\n");
        goto out;
    }
    /* the AudioSpecificConfig of the track tells the decoder the stream layout */
    ext.pInputBuffer = t->extra;
    ext.inputBufferCurrentLength = t->extra_len;
    ext.inputBufferUsedLength = 0;
    ext.remainderBits = 0;
    if (PVMP4AudioDecoderConfig(&ext, dec) != MP4AUDEC_SUCCESS)
    {
        rt_kprintf("mp4: bad AudioSpecificConfig\n");
        goto out;
    }
    if (rt_device_open(dev, RT_DEVICE_OFLAG_WRONLY) != RT_EOK)
    {
        rt_kprintf("mp4: cannot open the audio device\n");
        goto out;
    }
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = audio_rate;
    caps.udata.config.channels = 2;
    caps.udata.config.samplebits = 16;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type = AUDIO_MIXER_VOLUME;
    caps.udata.value = 70;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);

    mp4_iter_init(t, &it);
    while (!stop_req && mp4_next(t, &it, &s) == 0)
    {
        if (file_read(&afile, s.offset, buf, s.size) != 0)
        {
            rt_kprintf("mp4: audio read error at sample %u\n", s.index);
            break;
        }
        ext.pInputBuffer = buf;
        ext.inputBufferCurrentLength = s.size;
        ext.inputBufferUsedLength = 0;
        ext.remainderBits = 0;
        ret = PVMP4AudioDecodeFrame(&ext, dec);
        if (ret != MP4AUDEC_SUCCESS)
        {
            rt_kprintf("mp4: audio decode error %d at sample %u\n", ret, s.index);
            continue;
        }
        if (skip > 0)
        {
            /* the encoder delay is not part of the programme */
            skip--;
            continue;
        }
        /* desiredChannels is 2: a mono stream comes out duplicated */
        memcpy(fifo + fifo_len, pcm, PCM_BYTES);
        fifo_len += PCM_BYTES;
        while (fifo_len >= BLOCK_BYTES)
        {
            /* blocks until the codec has room: the writer is paced by the DAC */
            rt_device_write(dev, 0, fifo, BLOCK_BYTES);
            fifo_len -= BLOCK_BYTES;
            memmove(fifo, fifo + BLOCK_BYTES, fifo_len);
            if (!audio_running && sun252i_codec_dac_count() > 0)
                audio_running = RT_TRUE;
        }
    }
    rt_thread_mdelay(300);
    rt_device_control(dev, AUDIO_CTL_STOP, RT_NULL);
    rt_device_close(dev);
out:
    rt_free(dec);
    rt_free(pcm);
    rt_free(fifo);
    rt_free(buf);
    audio_running = RT_FALSE;
    audio_done = RT_TRUE;
}

/* ---- video ----------------------------------------------------------------------------- */

static rt_uint8_t annexb_head[512];
static size_t annexb_head_len;
static int nal_len_size;

/* SPS and PPS of the avcC record as Annex B */
static int avcc_parameter_sets(const rt_uint8_t *e, rt_uint32_t len)
{
    size_t o = 5, w = 0;
    int n, pass, i;

    if (len < 7U || e[0] != 1)
        return -EINVAL;
    nal_len_size = (e[4] & 3) + 1;
    for (pass = 0; pass < 2; pass++)
    {
        n = pass == 0 ? (e[o] & 0x1f) : e[o];
        o++;
        for (i = 0; i < n; i++)
        {
            size_t l;

            if (o + 2 > len)
                return -EINVAL;
            l = (e[o] << 8) | e[o + 1];
            o += 2;
            if (o + l > len || w + 4 + l > sizeof(annexb_head))
                return -EINVAL;
            annexb_head[w++] = 0;
            annexb_head[w++] = 0;
            annexb_head[w++] = 0;
            annexb_head[w++] = 1;
            memcpy(annexb_head + w, e + o, l);
            w += l;
            o += l;
        }
    }
    annexb_head_len = w;

    return 0;
}

/* length prefixed NAL units to start code prefixed ones; returns the new size */
static size_t avcc_to_annexb(const rt_uint8_t *in, size_t len, rt_uint8_t *out, rt_bool_t with_head)
{
    size_t w = 0;

    if (with_head)
    {
        memcpy(out, annexb_head, annexb_head_len);
        w = annexb_head_len;
    }
    while (len > (size_t)nal_len_size)
    {
        size_t l = 0;
        int i;

        for (i = 0; i < nal_len_size; i++)
            l = (l << 8) | *in++;
        len -= nal_len_size;
        if (l > len)
            break;
        out[w++] = 0;
        out[w++] = 0;
        out[w++] = 0;
        out[w++] = 1;
        memcpy(out + w, in, l);
        w += l;
        in += l;
        len -= l;
    }

    return w;
}

static rt_uint32_t shown_count, dropped_count;
static rt_int64_t last_offset_us;

static rt_int64_t now_us(void)
{
    return (rt_int64_t)rt_tick_get_millisecond() * 1000;
}

static void show(const struct vdec_frame *f)
{
    struct lcd_yuv yuv;

    yuv.y = f->plane[0];
    yuv.uv = f->plane[1];
    yuv.width = f->width;
    yuv.height = f->height;
    yuv.stride_y = f->stride[0];
    yuv.stride_uv = f->stride[1];
    yuv.bt709 = RT_TRUE;
    /* the picture is up at the next refresh, the decoder does not wait for it */
    yuv.nonblock = RT_TRUE;
    lcd_show_yuv(&yuv);
}

/* waits for the time of a picture; false when it is too late to show it */
static rt_bool_t wait_for(rt_int64_t pts_us, rt_int64_t start_us)
{
    for (;;)
    {
        rt_int64_t now = audio_clock_us();
        rt_int64_t diff;

        if (now < 0)
        {
            if (!audio_done)
            {
                rt_thread_mdelay(2);
                continue;
            }
            now = now_us() - start_us;
        }
        diff = pts_us - now;
        last_offset_us = -diff;
        if (diff < -DROP_US)
            return RT_FALSE;
        if (diff <= LEAD_US)
            return RT_TRUE;
        /* a tick is 10 ms: sleep the whole ticks, spin the rest */
        if (diff - LEAD_US > 12000)
            rt_thread_mdelay((rt_uint32_t)((diff - LEAD_US - 5000) / 1000));
        else
            rt_hw_us_delay(500);
    }
}

static void video_run(const char *path, struct stat *st)
{
    const struct mp4_track *v;
    struct vdec_stream_config scfg;
    struct vdec_frame frame, shown, older;
    struct vdec_stream *stream = RT_NULL;
    struct mp4_iter it;
    struct mp4_sample s;
    rt_uint8_t *raw = RT_NULL, *ab = RT_NULL;
    size_t ab_len = 0, ab_used = 0;
    rt_int64_t pending_pts = -1, start_us, last_report;
    rt_bool_t eof = RT_FALSE, flushed = RT_FALSE, first = RT_TRUE;
    rt_uint32_t report_frames = 0;
    rt_thread_t at = RT_NULL;
    int ret;

    memset(&shown, 0, sizeof(shown));
    memset(&older, 0, sizeof(older));
    memset(&scfg, 0, sizeof(scfg));
    scfg.codec = VDEC_CODEC_H264;
    scfg.format = VDEC_FORMAT_NV12;
    scfg.buffer_size = 1024 * 1024;

    vfile.win_size = 256 * 1024;
    afile.win_size = 64 * 1024;
    vfile.win = rt_malloc_align(vfile.win_size, 64);
    afile.win = rt_malloc_align(afile.win_size, 64);
    vfile.fd = open(path, O_RDONLY);
    afile.fd = open(path, O_RDONLY);
    if (!vfile.win || !afile.win || vfile.fd < 0 || afile.fd < 0)
    {
        rt_kprintf("mp4: cannot open %s\n", path);
        goto out;
    }
    ret = mp4_open(&mp4, file_read, &vfile, st->st_size);
    if (ret != 0 || mp4.video.codec != MP4_CODEC_H264)
    {
        rt_kprintf("mp4: %s has no usable video track (%d)\n", path, ret);
        goto out;
    }
    v = &mp4.video;
    if (avcc_parameter_sets(v->extra, v->extra_len) != 0)
    {
        rt_kprintf("mp4: bad avcC record\n");
        goto out;
    }
    rt_kprintf("mp4: playing %s: %ux%u, %u frames%s\n", path, v->width, v->height, v->sample_count,
               mp4.audio.codec == MP4_CODEC_AAC ? ", with sound" : ", no sound");

    audio_running = RT_FALSE;
    audio_done = RT_TRUE;
    audio_rate = mp4.audio.sample_rate;
    if (mp4.audio.codec == MP4_CODEC_AAC && (audio_rate == 48000U || audio_rate == 44100U))
    {
        audio_done = RT_FALSE;
        at = rt_thread_create("mp4audio", audio_main, RT_NULL, 16384, 6, 10);
        if (at)
            rt_thread_startup(at);
        else
            audio_done = RT_TRUE;
    }
    else if (mp4.audio.codec == MP4_CODEC_AAC)
    {
        rt_kprintf("mp4: the audio is %u Hz, only 48000 and 44100 Hz are played\n", audio_rate);
    }

    if (vdec_stream_open(&scfg, &stream) != 0)
    {
        rt_kprintf("mp4: cannot open the decoder\n");
        goto out;
    }
    raw = rt_malloc(v->max_sample);
    ab = rt_malloc(v->max_sample + 1024);
    if (!raw || !ab)
    {
        rt_kprintf("mp4: no memory\n");
        goto out;
    }
    mp4_iter_init(v, &it);
    start_us = last_report = now_us();

    while (!stop_req)
    {
        ret = vdec_stream_get_frame(stream, &frame);
        if (ret == 0)
        {
            rt_int64_t pts_us = frame.pts >= 0 ? frame.pts : 0;

            if (wait_for(pts_us, start_us))
            {
                show(&frame);
                /* the picture before the last may still be scanned out */
                if (older.priv != RT_NULL)
                    vdec_frame_release(&older);
                older = shown;
                shown = frame;
                shown_count++;
                report_frames++;
            }
            else
            {
                vdec_frame_release(&frame);
                dropped_count++;
            }
            if (now_us() - last_report >= REPORT_MS * 1000LL)
            {
                rt_uint32_t ms = (rt_uint32_t)((now_us() - last_report) / 1000);

                rt_kprintf("mp4: %u shown (%u.%02u fps), %u dropped, A/V offset %d ms\n", shown_count,
                           report_frames * 1000 / ms, (report_frames * 100000 / ms) % 100, dropped_count,
                           (int)(last_offset_us / 1000));
                report_frames = 0;
                last_report = now_us();
            }
            continue;
        }
        if (ret == -ENODATA)
            break;
        if (ret == -EBUSY)
        {
            rt_thread_mdelay(2);
            continue;
        }
        if (ret != -EAGAIN)
        {
            rt_kprintf("mp4: decode error %d\n", ret);
            break;
        }

        /* the decoder wants data */
        if (ab_used >= ab_len)
        {
            if (mp4_next(v, &it, &s) != 0)
            {
                eof = RT_TRUE;
            }
            else
            {
                if (file_read(&vfile, s.offset, raw, s.size) != 0)
                {
                    rt_kprintf("mp4: video read error at sample %u\n", s.index);
                    break;
                }
                ab_len = avcc_to_annexb(raw, s.size, ab, first || s.sync);
                ab_used = 0;
                first = RT_FALSE;
                pending_pts = (s.pts - v->media_start) * 1000000 / v->timescale;
                if (pending_pts < 0)
                    pending_pts = 0;
            }
        }
        if (ab_used < ab_len)
        {
            size_t used = 0;

            ret = vdec_stream_feed(stream, ab + ab_used, ab_len - ab_used, pending_pts, &used);
            if (ret != 0 && ret != -EAGAIN)
            {
                rt_kprintf("mp4: feed error %d\n", ret);
                ab_used = ab_len;
            }
            else
            {
                ab_used += used;
            }
        }
        else if (eof && !flushed)
        {
            vdec_stream_flush(stream);
            flushed = RT_TRUE;
        }
    }
    if (older.priv != RT_NULL)
        vdec_frame_release(&older);
    if (shown.priv != RT_NULL)
        vdec_frame_release(&shown);
    rt_kprintf("mp4: done, %u shown, %u dropped\n", shown_count, dropped_count);

out:
    stop_req = RT_TRUE;
    while (at && !audio_done)
        rt_thread_mdelay(50);
    if (stream)
        vdec_stream_close(stream);
    lcd_hide_yuv();
    rt_free(raw);
    rt_free(ab);
    if (vfile.fd >= 0)
        close(vfile.fd);
    if (afile.fd >= 0)
        close(afile.fd);
    if (vfile.win)
        rt_free_align(vfile.win);
    if (afile.win)
        rt_free_align(afile.win);
    mp4_close(&mp4);
}

static char play_path[128];

static void player_thread(void *arg)
{
    struct stat st;

    shown_count = dropped_count = 0;
    vfile.fd = afile.fd = -1;
    vfile.win = afile.win = RT_NULL;
    if (mount_card() != 0)
        rt_kprintf("mp4: no file system on the SD card\n");
    else if (stat(play_path, &st) != 0)
        rt_kprintf("mp4: %s not found\n", play_path);
    else
        video_run(play_path, &st);
    playing = RT_FALSE;
}

static int mp4_play(int argc, char **argv)
{
    rt_thread_t t;

    if (argc < 2)
    {
        rt_kprintf("usage: mp4_play <file>\n");
        return -1;
    }
    if (playing)
    {
        rt_kprintf("mp4: already playing, mp4_stop first\n");
        return -1;
    }
    if (!lcd_framebuffer())
    {
        rt_kprintf("mp4: the display is not ready\n");
        return -1;
    }
    rt_strncpy(play_path, argv[1], sizeof(play_path) - 1);
    stop_req = RT_FALSE;
    playing = RT_TRUE;
    t = rt_thread_create("mp4", player_thread, RT_NULL, 16384, 9, 10);
    if (!t)
    {
        playing = RT_FALSE;
        return -1;
    }
    rt_thread_startup(t);

    return 0;
}
MSH_CMD_EXPORT(mp4_play, play an MP4 file: mp4_play <file>);

static int mp4_stop(int argc, char **argv)
{
    stop_req = RT_TRUE;

    return 0;
}
MSH_CMD_EXPORT(mp4_stop, stop the MP4 playback);
