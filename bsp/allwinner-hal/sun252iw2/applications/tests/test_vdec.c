/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Video engine: a JPEG and a PNG picture are decoded and compared with the
 * host decode (PNG exact, JPEG by the mean luma of 16x16 blocks), and an H.264
 * clip is decoded frame by frame against the CRC32 of every frame.
 */
#include <rtthread.h>
#include <errno.h>
#include <string.h>
#include <sunxi-util.h>
#include <vdec/vdec.h>
#include "test.h"
#include "data/test_jpeg.h"
#include "data/test_png.h"
#include "data/test_h264.h"
#include "data/test_ref.h"

static rt_uint32_t crc32_update(rt_uint32_t crc, const rt_uint8_t *p, rt_uint32_t len)
{
    crc = ~crc;
    while (len--)
    {
        int k;

        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static rt_uint32_t ms_now(void)
{
    return (rt_uint32_t)(((rt_uint64_t)rt_tick_get() * 1000u) / RT_TICK_PER_SECOND);
}

static void test_jpeg_picture(struct test_ctx *c)
{
    struct vdec_frame f;
    rt_uint32_t t, bx, by, worst = 0, r, x;
    int ret;

    t = ms_now();
    ret = vdec_decode_image(VDEC_CODEC_JPEG, test_jpeg, sizeof(test_jpeg), VDEC_FORMAT_NV12, &f);
    t = ms_now() - t;
    TEST_CHECK(c, ret == 0, "jpeg decode");
    if (ret)
        return;
    rt_kprintf("vdec: jpeg %ux%u stride %u/%u in %u ms\n", f.width, f.height, f.stride[0], f.stride[1], t);
    TEST_CHECK(c, f.width == TEST_JPEG_WIDTH && f.height == TEST_JPEG_HEIGHT && f.format == VDEC_FORMAT_NV12,
               "jpeg size and format");
    for (by = 0; by < TEST_JPEG_HEIGHT / 16; by++)
    {
        for (bx = 0; bx < TEST_JPEG_WIDTH / 16; bx++)
        {
            rt_uint32_t sum = 0, mean, ref, d;

            for (r = 0; r < 16; r++)
                for (x = 0; x < 16; x++)
                    sum += f.plane[0][(by * 16 + r) * f.stride[0] + bx * 16 + x];
            mean = sum / 256;
            ref = test_jpeg_blocks[by * (TEST_JPEG_WIDTH / 16) + bx];
            d = mean > ref ? mean - ref : ref - mean;
            if (d > worst)
                worst = d;
        }
    }
    rt_kprintf("vdec: jpeg worst block luma difference %u\n", worst);
    TEST_CHECK(c, worst <= 3, "jpeg luma matches the host decode");
    vdec_frame_release(&f);
}

static void test_png_picture(struct test_ctx *c)
{
    struct vdec_frame f;
    rt_uint32_t crc = 0, y, t;
    int ret;

    t = ms_now();
    ret = vdec_decode_image(VDEC_CODEC_PNG, test_png, sizeof(test_png), VDEC_FORMAT_RGBA8888, &f);
    t = ms_now() - t;
    TEST_CHECK(c, ret == 0, "png decode");
    if (ret)
        return;
    rt_kprintf("vdec: png %ux%u in %u ms\n", f.width, f.height, t);
    for (y = 0; y < f.height; y++)
        crc = crc32_update(crc, f.plane[0] + y * f.stride[0], f.width * 4u);
    rt_kprintf("vdec: png crc %08x\n", crc);
    TEST_CHECK(c, crc == TEST_PNG_CRC, "png equals the host decode");
    vdec_frame_release(&f);
}

static rt_uint32_t frame_crc(const struct vdec_frame *f)
{
    rt_uint32_t crc = 0, y;

    for (y = 0; y < f->height; y++)
        crc = crc32_update(crc, f->plane[0] + y * f->stride[0], f->width);
    for (y = 0; y < f->height / 2u; y++)
        crc = crc32_update(crc, f->plane[1] + y * f->stride[1], f->width);
    return crc;
}

static void test_h264_clip(struct test_ctx *c)
{
    const struct vdec_stream_config cfg = { .codec = VDEC_CODEC_H264, .format = VDEC_FORMAT_NV12 };
    struct vdec_stream *st;
    struct vdec_frame f;
    size_t off = 0, used;
    rt_uint32_t frames = 0, bad = 0, t;
    int ret;

    ret = vdec_stream_open(&cfg, &st);
    TEST_CHECK(c, ret == 0, "h264 stream open");
    if (ret)
        return;
    t = ms_now();
    for (;;)
    {
        ret = vdec_stream_get_frame(st, &f);
        if (ret == -EAGAIN && off < sizeof(test_h264))
        {
            ret = vdec_stream_feed(st, test_h264 + off, sizeof(test_h264) - off, frames * 33, &used);
            if (ret != 0 && ret != -EAGAIN)
            {
                rt_kprintf("vdec: feed failed %d\n", ret);
                break;
            }
            off += used;
            if (off >= sizeof(test_h264))
                vdec_stream_flush(st);
            continue;
        }
        if (ret == -ENODATA)
            break;
        if (ret != 0)
        {
            rt_kprintf("vdec: get_frame failed %d at frame %u\n", ret, frames);
            break;
        }
        if (frames < sizeof(test_h264_crc) / sizeof(test_h264_crc[0]))
        {
            rt_uint32_t crc = frame_crc(&f);

            if (crc != test_h264_crc[frames])
            {
                bad++;
                rt_kprintf("vdec: frame %u crc %08x expected %08x\n", frames, crc, test_h264_crc[frames]);
            }
        }
        else
            bad++;
        frames++;
        vdec_frame_release(&f);
    }
    t = ms_now() - t;
    rt_kprintf("vdec: h264 %u frames in %u ms, %u bad\n", frames, t, bad);
    TEST_CHECK(c, frames == sizeof(test_h264_crc) / sizeof(test_h264_crc[0]), "h264 frame count");
    TEST_CHECK(c, bad == 0, "h264 frames equal the reference CRCs");
    vdec_stream_close(st);
}

static int test_vdec(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    test_jpeg_picture(&c);
    test_png_picture(&c);
    test_h264_clip(&c);
    return test_summary("vdec", &c);
}
MSH_CMD_EXPORT(test_vdec, video engine: JPEG/PNG/H.264 decode against the host reference);
