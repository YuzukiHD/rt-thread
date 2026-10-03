/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * H.264 decode speed of a raw Annex B file of the SD card: no sound, no picture, no pacing. The file is cut into
 * pictures, the decode of each is timed with the 24 MHz counter, and the video engine memory traffic is read from the
 * bus counters. Needs BSP_USING_BENCH.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <drivers/clock_time.h>

#include "mbus-sun252i.h"
#include "vdec.h"

#define MBUS_PMU_VE         4
#define BENCH_REPORT_MS     5000

static char bench_path[128];
static volatile rt_bool_t bench_stop_req, bench_running;
static unsigned int bench_ve_mhz, bench_max_frames, bench_no_cache;

/* the card is mounted on / (partition 1, or the whole device) */
static int bench_mount_card(void)
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
            rt_kprintf("bench: %s mounted on /\n", names[i]);
            return 0;
        }
    }

    return stat("/", &st) == 0 ? 0 : -1;
}

#define RAW_CHUNK       (512 * 1024)

/* microseconds from the 24 MHz counter (the kernel tick is too coarse for one picture) */
static rt_uint64_t hr_us(void)
{
    return rt_clock_time_get_counter() * 1000000ull / rt_clock_time_get_freq();
}

struct nal_bits
{
    const rt_uint8_t *p;
    int n, pos;
};

static unsigned bits_ue(struct nal_bits *b)
{
    int zeros = 0, i;
    unsigned v = 0;

    while (b->pos < b->n * 8 && !((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1))
    {
        zeros++;
        b->pos++;
    }
    b->pos++;
    for (i = 0; i < zeros && b->pos < b->n * 8; i++)
    {
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }

    return ((1u << zeros) - 1u) + v;    /* Exp-Golomb: 2^zeros - 1 plus the suffix bits */
}

/* first_mb_in_slice and slice_type of the slice NAL unit whose header byte is at @nal */
static void slice_info(const rt_uint8_t *nal, int len, unsigned *first_mb, unsigned *slice_type)
{
    rt_uint8_t raw[16];
    int i, n = 0, zeros = 0;
    struct nal_bits b;

    for (i = 1; i < len && n < (int)sizeof(raw); i++)
    {
        if (zeros >= 2 && nal[i] == 3)      /* emulation prevention byte */
        {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        raw[n++] = nal[i];
    }
    b.p = raw;
    b.n = n;
    b.pos = 0;
    *first_mb = bits_ue(&b);
    *slice_type = bits_ue(&b);
}

/* the next 00 00 01 at or after @i, or @end */
static int find_sc(const rt_uint8_t *buf, int i, int end)
{
    for (; i + 2 < end; i++)
    {
        if (buf[i + 2] > 1)
            i += 2;
        else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            return i;
    }

    return end;
}

enum frame_class { FC_I, FC_P, FC_B, FC_COUNT };

/*
 * The access unit (one picture: parameter sets, SEI, the slices) that starts at @pos. Returns its end, or
 * -1 when the buffer ends inside it and more of the file is needed (@eof: the end of the file ends it).
 */
static int next_au(const rt_uint8_t *buf, int pos, int have, rt_bool_t eof, enum frame_class *cls, rt_bool_t *idr)
{
    int sc = find_sc(buf, pos, have);
    rt_bool_t vcl_seen = RT_FALSE;

    *cls = FC_P;
    *idr = RT_FALSE;
    while (sc < have)
    {
        int next = find_sc(buf, sc + 3, have), nal = sc + 3, type;

        if (next >= have && !eof)
            return -1;                          /* this NAL unit may go on in the next part of the file */
        if (nal >= have)
            break;
        type = buf[nal] & 0x1f;
        if (type == 1 || type == 5)
        {
            unsigned first_mb, slice_type;

            slice_info(buf + nal, next - nal, &first_mb, &slice_type);
            if (vcl_seen && first_mb == 0)
                return sc;                      /* the first slice of the next picture */
            if (!vcl_seen)
            {
                slice_type %= 5;
                *cls = (slice_type == 2 || slice_type == 4) ? FC_I : (slice_type == 1 ? FC_B : FC_P);
                *idr = type == 5;
            }
            vcl_seen = RT_TRUE;
        }
        else if (vcl_seen && (type == 6 || type == 7 || type == 8 || type == 9))
        {
            return sc;
        }
        sc = next;
    }

    return eof ? have : -1;
}

struct class_stat
{
    rt_uint32_t count, idr;
    rt_uint64_t total_us, min_us, max_us, bytes;
};

static void h264_bench_thread(void *arg)
{
    static const char *const names[FC_COUNT] = { "I", "P", "B" };
    struct class_stat stat_[FC_COUNT];
    struct stat st;
    struct vdec_stream *stream = RT_NULL;
    struct vdec_stream_config cfg;
    struct vdec_frame f;
    rt_uint8_t *buf = rt_malloc_align(RAW_CHUNK, 64);
    int have = 0, pos = 0, fd = -1, ret, end, i;
    rt_uint64_t t0, t_report, read_us = 0, t, ve_total = 0;
    rt_uint32_t frames = 0, report_frames = 0, w = 0, h = 0, report_ve0;
    rt_size_t total, in_use, max_use;
    rt_bool_t eof = RT_FALSE, flushed = RT_FALSE, idr;
    enum frame_class cls;

    memset(stat_, 0, sizeof(stat_));
    for (i = 0; i < FC_COUNT; i++)
        stat_[i].min_us = ~0ull;
    if (bench_mount_card() != 0 || stat(bench_path, &st) != 0 || (fd = open(bench_path, O_RDONLY)) < 0 || !buf)
    {
        rt_kprintf("h264 bench: cannot open %s\n", bench_path);
        goto out;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.codec = VDEC_CODEC_H264;
    cfg.format = VDEC_FORMAT_NV12;
    cfg.buffer_size = 1024 * 1024;
    cfg.no_cache_ops = bench_no_cache;
    if (vdec_stream_open(&cfg, &stream) != 0)
    {
        rt_kprintf("h264 bench: cannot open the decoder\n");
        goto out;
    }
    if (bench_ve_mhz)
    {
        int set = vdec_set_clock_hz(bench_ve_mhz * 1000000u);

        rt_kprintf("h264 bench: video engine clock %u MHz requested, %d Hz set\n", bench_ve_mhz, set);
    }
    rt_kprintf("h264 bench: %s, video engine %u Hz, cache operations %s\n", bench_path, vdec_clock_hz(),
               cfg.no_cache_ops ? "off" : "on");
    t0 = t_report = hr_us();
    report_ve0 = mbus_traffic(MBUS_PMU_VE);
    while (!bench_stop_req && (!bench_max_frames || frames < bench_max_frames))
    {
        end = have > pos ? next_au(buf, pos, have, eof, &cls, &idr) : -1;
        if (end < 0)
        {
            if (!eof)
            {
                ssize_t n;

                if (have - pos >= RAW_CHUNK)
                {
                    rt_kprintf("h264 bench: a picture larger than %u bytes\n", RAW_CHUNK);
                    break;
                }
                memmove(buf, buf + pos, have - pos);
                have -= pos;
                pos = 0;
                t = hr_us();
                n = read(fd, buf + have, RAW_CHUNK - have);
                read_us += hr_us() - t;
                if (n <= 0)
                    eof = RT_TRUE;
                else
                    have += (int)n;
                continue;
            }
            if (!flushed)
            {
                vdec_stream_flush(stream);
                flushed = RT_TRUE;
            }
        }
        else
        {
            /* hand the picture over, drain the decoder: the time spent in vdec_stream_get_frame() is the decode time */
            rt_uint64_t decode_us = 0;
            int au_pos = pos;
            size_t used;

            while (au_pos < end && !bench_stop_req)
            {
                ret = vdec_stream_feed(stream, buf + au_pos, end - au_pos, -1, &used);
                if (ret == 0)
                    au_pos += (int)used;
                else if (ret != -EAGAIN)
                {
                    rt_kprintf("h264 bench: feed error %d\n", ret);
                    au_pos = end;
                    break;
                }
                for (;;)
                {
                    t = hr_us();
                    ret = vdec_stream_get_frame(stream, &f);
                    decode_us += hr_us() - t;
                    if (ret == 0)
                    {
                        w = f.width;
                        h = f.height;
                        vdec_frame_release(&f);
                        frames++;
                        report_frames++;
                        continue;
                    }
                    if (ret == -EBUSY)
                    {
                        rt_thread_mdelay(1);
                        continue;
                    }
                    break;
                }
                if (ret != -EAGAIN)
                    break;
            }
            stat_[cls].count++;
            stat_[cls].idr += idr;
            stat_[cls].total_us += decode_us;
            stat_[cls].bytes += end - pos;
            stat_[cls].min_us = decode_us < stat_[cls].min_us ? decode_us : stat_[cls].min_us;
            stat_[cls].max_us = decode_us > stat_[cls].max_us ? decode_us : stat_[cls].max_us;
            pos = end;
            if (hr_us() - t_report >= BENCH_REPORT_MS * 1000ull)
            {
                rt_uint32_t ms = (rt_uint32_t)((hr_us() - t_report) / 1000), ve = mbus_traffic(MBUS_PMU_VE);

                rt_kprintf("h264 bench: %u frames, %u.%02u fps over the last %u ms, video engine %u MB/s\n", frames,
                           report_frames * 1000 / ms, (report_frames * 100000 / ms) % 100, ms,
                           (rt_uint32_t)((ve - report_ve0) / 1024u * 1000u / ms / 1024u));
                ve_total += (rt_uint32_t)(ve - report_ve0);    /* the counter is 32 bit: add up every window */
                report_frames = 0;
                report_ve0 = ve;
                t_report = hr_us();
            }
            continue;
        }
        /* end of the file: the pictures still inside the decoder */
        t = hr_us();
        ret = vdec_stream_get_frame(stream, &f);
        if (ret == 0)
        {
            vdec_frame_release(&f);
            frames++;
            continue;
        }
        if (ret == -EBUSY)
        {
            rt_thread_mdelay(1);
            continue;
        }
        break;
    }
    t = hr_us() - t0;
    rt_memory_info(&total, &in_use, &max_use);
    rt_kprintf("h264 bench: %ux%u, %u frames in %u ms = %u fps, file read %u ms, heap peak %u KB of %u KB\n", w, h,
               frames, (rt_uint32_t)(t / 1000), (rt_uint32_t)(frames * 1000000ull / (t ? t : 1)),
               (rt_uint32_t)(read_us / 1000), (rt_uint32_t)(max_use / 1024), (rt_uint32_t)(total / 1024));
    for (i = 0; i < FC_COUNT; i++)
    {
        struct class_stat *c = &stat_[i];

        if (!c->count)
            continue;
        rt_kprintf("h264 bench: %s pictures %5u (%u IDR): decode avg %u us, min %u, max %u, avg size %u KB, "
                   "%u.%02u fps if all were %s\n", names[i], c->count, c->idr, (rt_uint32_t)(c->total_us / c->count),
                   (rt_uint32_t)c->min_us, (rt_uint32_t)c->max_us, (rt_uint32_t)(c->bytes / c->count / 1024),
                   (rt_uint32_t)(c->count * 1000000ull / c->total_us), (rt_uint32_t)((c->count * 100000000ull / c->total_us) % 100),
                   names[i]);
    }
    ve_total += (rt_uint32_t)(mbus_traffic(MBUS_PMU_VE) - report_ve0);
    rt_kprintf("h264 bench: video engine traffic %u MB in %u ms = %u MB/s, %u KB per picture\n",
               (rt_uint32_t)(ve_total / 1048576u), (rt_uint32_t)(t / 1000),
               (rt_uint32_t)(ve_total * 1000000ull / 1048576u / (t ? t : 1)),
               frames ? (rt_uint32_t)(ve_total / 1024u / frames) : 0);
out:
    if (stream)
        vdec_stream_close(stream);
    if (fd >= 0)
        close(fd);
    rt_free_align(buf);
    bench_running = RT_FALSE;
}

static int h264_bench(int argc, char **argv)
{
    rt_thread_t t;

    if (argc < 2)
    {
        rt_kprintf("usage: h264_bench <raw Annex B file> [ve_mhz [max_frames [no_cache]]]\n");
        return -1;
    }
    if (bench_running)
    {
        rt_kprintf("h264 bench: busy\n");
        return -1;
    }
    rt_strncpy(bench_path, argv[1], sizeof(bench_path) - 1);
    bench_ve_mhz = argc > 2 ? atoi(argv[2]) : 0;
    bench_max_frames = argc > 3 ? atoi(argv[3]) : 0;
    bench_no_cache = argc > 4 ? atoi(argv[4]) : 0;
    bench_stop_req = RT_FALSE;
    bench_running = RT_TRUE;
    t = rt_thread_create("h264b", h264_bench_thread, RT_NULL, 8192, 9, 10);
    if (!t)
    {
        bench_running = RT_FALSE;
        return -1;
    }
    rt_thread_startup(t);

    return 0;
}
MSH_CMD_EXPORT(h264_bench, decode speed per picture type of a raw H.264 file: h264_bench <file> [ve_mhz [max_frames [no_cache]]]);

static int bench_stop(int argc, char **argv)
{
    bench_stop_req = RT_TRUE;

    return 0;
}
MSH_CMD_EXPORT(bench_stop, end a running h264_bench and print its summary);
