/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runs every operation of the G2D on small buffers and compares the result
 * with a CPU implementation (exact for copies, rotations and fills, within a
 * tolerance for scaling and blending).
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <g2d/g2d.h>
#define LOG_I(fmt, ...) rt_kprintf("g2d: " fmt "\n", ##__VA_ARGS__)
#define LOG_E(fmt, ...) rt_kprintf("g2d: " fmt "\n", ##__VA_ARGS__)
#define sys_cache_data_flush_range sunxi_dcache_flush
#define aligned_alloc(a, s) rt_malloc_align((s), (a))
#define free(p) rt_free_align(p)

#define W	128
#define H	96

static int failures;

static void *alloc_buf(size_t size)
{
	void *p = aligned_alloc(64, ROUND_UP(size, 64));

	RT_ASSERT(p != NULL);
	return p;
}

static struct g2d_surface surface(enum g2d_format fmt, uint16_t w, uint16_t h)
{
	struct g2d_surface s = { .format = fmt, .width = w, .height = h };

	s.plane[0] = alloc_buf((size_t)w * h * g2d_format_bytes_per_pixel(fmt));
	return s;
}

static void release(struct g2d_surface *s)
{
	rt_free_align(s->plane[0]);
}

static uint32_t *px32(const struct g2d_surface *s, int x, int y)
{
	return (uint32_t *)((uint8_t *)s->plane[0] + ((size_t)y * s->width + x) * 4);
}

static uint16_t *px16(const struct g2d_surface *s, int x, int y)
{
	return (uint16_t *)((uint8_t *)s->plane[0] + ((size_t)y * s->width + x) * 2);
}

/* test pattern: gradients and an alpha ramp, distinct per pixel */
static uint32_t pattern(int x, int y, uint8_t alpha)
{
	return ((uint32_t)alpha << 24) | ((x * 255 / W) << 16) | ((y * 255 / H) << 8) |
	       ((x + y) & 0xff);
}

static void fill_pattern(const struct g2d_surface *s, uint8_t alpha)
{
	for (int y = 0; y < s->height; y++) {
		for (int x = 0; x < s->width; x++) {
			*px32(s, x, y) = pattern(x, y, alpha);
		}
	}
	sys_cache_data_flush_range(s->plane[0], (size_t)s->width * s->height * 4);
}

static uint16_t to565(uint32_t argb)
{
	return ((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) | ((argb >> 3) & 0x001f);
}

static int chan_diff(uint32_t a, uint32_t b)
{
	int d = 0;

	for (int sh = 0; sh < 32; sh += 8) {
		d = MAX(d, abs((int)((a >> sh) & 0xff) - (int)((b >> sh) & 0xff)));
	}
	return d;
}

static uint32_t t0;

static void begin(void)
{
	t0 = (*(volatile uint32_t *)0x1400BFF8u);
}

static void report(const char *name, bool ok, int worst, int rc)
{
	uint32_t us = ((*(volatile uint32_t *)0x1400BFF8u) - t0) / 24;

	if (ok) {
		LOG_I("%-28s PASS  max diff %3d  %u us", name, worst, us);
	} else {
		LOG_E("%-28s FAIL  max diff %3d  rc %d", name, worst, rc);
		failures++;
	}
}

static void test_fill(enum g2d_format fmt, const char *name)
{
	struct g2d_surface d = surface(fmt, W, H);
	struct g2d_rect r = { 16, 8, 64, 48 };
	const uint32_t color = 0xff3080c0;
	bool bpp4 = g2d_format_bytes_per_pixel(fmt) == 4;
	bool ok = true;
	int rc;

	memset(d.plane[0], 0, (size_t)W * H * g2d_format_bytes_per_pixel(fmt));
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * g2d_format_bytes_per_pixel(fmt));
	begin();
	rc = g2d_fill(&d, &r, color);
	for (int y = 0; ok && y < H; y++) {
		for (int x = 0; x < W; x++) {
			bool in = x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;

			if (bpp4) {
				uint32_t want = in ? color : 0;

				if (fmt == G2D_PIXFMT_XRGB8888) {
					want &= 0x00ffffff;
				}
				if (chan_diff(*px32(&d, x, y), want) != 0 &&
				    !(fmt == G2D_PIXFMT_XRGB8888 &&
				      (*px32(&d, x, y) & 0xffffff) == (want & 0xffffff))) {
					ok = false;
					break;
				}
			} else if (*px16(&d, x, y) != (in ? to565(color) : 0)) {
				ok = false;
				break;
			}
		}
	}
	report(name, ok && rc == 0, ok ? 0 : 255, rc);
	release(&d);
}

static void test_blit_copy(void)
{
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_rect sr = { 10, 6, 80, 60 }, dr = { 30, 20, 80, 60 };
	bool ok = true;
	int rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blit(&s, &sr, &d, &dr, G2D_ROTATE_0, 0);
	for (int y = 0; ok && y < sr.height; y++) {
		for (int x = 0; x < sr.width; x++) {
			if (*px32(&d, dr.x + x, dr.y + y) != *px32(&s, sr.x + x, sr.y + y)) {
				ok = false;
				break;
			}
		}
	}
	report("blit ARGB8888 copy", ok && rc == 0, ok ? 0 : 255, rc);
	release(&s);
	release(&d);
}

static void test_blit_convert(void)
{
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_RGB565, W, H);
	struct g2d_rect r = { 0, 0, W, H };
	int worst = 0, rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 2);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 2);
	begin();
	rc = g2d_blit(&s, &r, &d, &r, G2D_ROTATE_0, 0);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint16_t want = to565(*px32(&s, x, y));
			uint16_t got = *px16(&d, x, y);
			int dr_ = abs((int)(want >> 11) - (int)(got >> 11));
			int dg = abs((int)((want >> 5) & 63) - (int)((got >> 5) & 63));
			int db = abs((int)(want & 31) - (int)(got & 31));

			worst = MAX(worst, MAX(dr_, MAX(dg, db)));
		}
	}
	report("blit ARGB8888 -> RGB565", rc == 0 && worst <= 1, worst, rc);
	release(&s);
	release(&d);
}

static void test_scale(void)
{
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_rect sr = { 0, 0, W / 2, H / 2 }, dr = { 0, 0, W, H };
	int worst = 0, rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blit(&s, &sr, &d, &dr, G2D_ROTATE_0, 0);
	/* smooth gradients: every output pixel is close to the source pixel it covers */
	for (int y = 2; y < H - 2; y++) {
		for (int x = 2; x < W - 2; x++) {
			worst = MAX(worst, chan_diff(*px32(&d, x, y), *px32(&s, x / 2, y / 2)));
		}
	}
	report("blit scale 2x", rc == 0 && worst <= 24, worst, rc);
	release(&s);
	release(&d);
}

static void test_rotate(enum g2d_rotation rot, uint32_t flags, const char *name)
{
	bool swap = rot == G2D_ROTATE_90 || rot == G2D_ROTATE_270;
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, swap ? H : W, swap ? W : H);
	struct g2d_rect sr = { 0, 0, W, H }, dr = { 0, 0, d.width, d.height };
	bool ok = true;
	int rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blit(&s, &sr, &d, &dr, rot, flags);
	for (int y = 0; ok && y < d.height; y++) {
		for (int x = 0; x < d.width; x++) {
			int sx, sy;
			int fx = (flags & G2D_FLIP_H) ? d.width - 1 - x : x;
			int fy = (flags & G2D_FLIP_V) ? d.height - 1 - y : y;

			/* rotate first, then flip: undo the flip, then the rotation */
			switch (rot) {
			case G2D_ROTATE_90:
				sx = fy;
				sy = H - 1 - fx;
				break;
			case G2D_ROTATE_180:
				sx = W - 1 - fx;
				sy = H - 1 - fy;
				break;
			case G2D_ROTATE_270:
				sx = W - 1 - fy;
				sy = fx;
				break;
			default:
				sx = fx;
				sy = fy;
				break;
			}
			if (*px32(&d, x, y) != *px32(&s, sx, sy)) {
				ok = false;
				break;
			}
		}
	}
	report(name, ok && rc == 0, ok ? 0 : 255, rc);
	release(&s);
	release(&d);
}


/* solid colour in limited range BT.601 YCbCr */
struct yuv_color {
	uint8_t y, u, v;
	uint32_t argb;
};

static const struct yuv_color yuv_colors[] = {
	{ 235, 128, 128, 0xffffffff },	/* white */
	{ 16, 128, 128, 0xff000000 },	/* black */
	{ 81, 90, 240, 0xffff0000 },	/* red */
	{ 145, 54, 34, 0xff00ff00 },	/* green */
	{ 41, 240, 110, 0xff0000ff },	/* blue */
};

/* the colours sit in horizontal bands so that a swapped U/V order shows up */
static void test_yuv(enum g2d_format fmt, const char *name)
{
	const int w = 64, h = 40, band = h / ARRAY_SIZE(yuv_colors);
	struct g2d_surface s = { .format = fmt, .width = w, .height = h };
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, w, h);
	struct g2d_rect r = { 0, 0, w, h };
	size_t size = (size_t)w * h * 2;
	uint8_t *buf = alloc_buf(size);
	uint8_t *y = buf, *u = NULL, *v = NULL;
	int worst = 0, rc;

	memset(buf, 0, size);
	s.plane[0] = buf;
	for (int row = 0; row < h; row++) {
		const struct yuv_color *c = &yuv_colors[MIN(row / band, ARRAY_SIZE(yuv_colors) - 1)];

		switch (fmt) {
		case G2D_PIXFMT_YUYV:
			for (int x = 0; x < w; x += 2) {
				uint8_t *p = buf + ((size_t)row * w + x) * 2;

				p[0] = c->y;
				p[1] = c->u;
				p[2] = c->y;
				p[3] = c->v;
			}
			break;
		default:
			memset(y + (size_t)row * w, c->y, w);
			break;
		}
	}
	if (fmt == G2D_PIXFMT_NV12 || fmt == G2D_PIXFMT_NV21) {
		uint8_t *uv = buf + (size_t)w * h;

		s.plane[1] = uv;
		for (int row = 0; row < h / 2; row++) {
			const struct yuv_color *c =
				&yuv_colors[MIN(row * 2 / band, ARRAY_SIZE(yuv_colors) - 1)];

			for (int x = 0; x < w; x += 2) {
				uv[(size_t)row * w + x] = fmt == G2D_PIXFMT_NV12 ? c->u : c->v;
				uv[(size_t)row * w + x + 1] = fmt == G2D_PIXFMT_NV12 ? c->v : c->u;
			}
		}
	} else if (fmt == G2D_PIXFMT_I420) {
		u = buf + (size_t)w * h;
		v = u + (size_t)(w / 2) * (h / 2);
		s.plane[1] = u;
		s.plane[2] = v;
		for (int row = 0; row < h / 2; row++) {
			const struct yuv_color *c =
				&yuv_colors[MIN(row * 2 / band, ARRAY_SIZE(yuv_colors) - 1)];

			memset(u + (size_t)row * (w / 2), c->u, w / 2);
			memset(v + (size_t)row * (w / 2), c->v, w / 2);
		}
	}
	sys_cache_data_flush_range(buf, size);
	memset(d.plane[0], 0, (size_t)w * h * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)w * h * 4);

	begin();
	rc = g2d_blit(&s, &r, &d, &r, G2D_ROTATE_0, 0);
	/* skip the rows next to a band edge, where the chroma is shared with the next band */
	for (int row = 0; row < h; row++) {
		const struct yuv_color *c = &yuv_colors[MIN(row / band, ARRAY_SIZE(yuv_colors) - 1)];

		if (row % band < 2 || row % band >= band - 2) {
			continue;
		}
		for (int x = 0; x < w; x++) {
			worst = MAX(worst, chan_diff(*px32(&d, x, row) | 0xff000000, c->argb));
		}
	}
	if (worst > 8) {
		LOG_I("%s: rows %08x %08x %08x %08x %08x", name, *px32(&d, 3, 4), *px32(&d, 3, 12),
			*px32(&d, 3, 20), *px32(&d, 3, 28), *px32(&d, 3, 36));
	}
	report(name, rc == 0 && worst <= 8, worst, rc);
	free(buf);
	release(&d);
}

static void test_blend(enum g2d_alpha_mode mode, uint8_t alpha, const char *name)
{
	struct g2d_surface fg = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface bg = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_rect r = { 0, 0, W, H };
	struct g2d_blend b = {
		.mode = G2D_BLEND_SRC_OVER,
		.fg_alpha_mode = mode,
		.fg_alpha = alpha,
		.bg_alpha_mode = G2D_ALPHA_PIXEL,
		.bg_alpha = 0xff,
	};
	int worst = 0, rc;

	fill_pattern(&fg, 0x80);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			*px32(&bg, x, y) = 0xff000000 | ((255 - x * 2) << 16) | (y << 8) | 0x40;
		}
	}
	sys_cache_data_flush_range(bg.plane[0], (size_t)W * H * 4);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blend(&fg, &r, &bg, &r, &d, &r, &b, 0);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t f = *px32(&fg, x, y), g = *px32(&bg, x, y), want = 0xff000000;
			unsigned int a = f >> 24;

			if (mode == G2D_ALPHA_GLOBAL) {
				a = alpha;
			} else if (mode == G2D_ALPHA_MIXED) {
				a = a * alpha / 255;
			}
			for (int sh = 0; sh < 24; sh += 8) {
				unsigned int fc = (f >> sh) & 0xff, gc = (g >> sh) & 0xff;

				want |= ((fc * a + gc * (255 - a)) / 255) << sh;
			}
			worst = MAX(worst, chan_diff(*px32(&d, x, y) | 0xff000000, want));
		}
	}
	report(name, rc == 0 && worst <= 3, worst, rc);
	release(&fg);
	release(&bg);
	release(&d);
}

static int test_g2d(int argc, char **argv)
{
	failures = 0;
	if (!g2d_ready()) {
		LOG_E("no G2D device");
		return -ENODEV;
	}

	test_fill(G2D_PIXFMT_ARGB8888, "fill ARGB8888");
	test_fill(G2D_PIXFMT_XRGB8888, "fill XRGB8888");
	test_fill(G2D_PIXFMT_RGB565, "fill RGB565");
	test_blit_copy();
	test_blit_convert();
	test_scale();
	test_rotate(G2D_ROTATE_0, G2D_FLIP_H, "flip horizontal");
	test_rotate(G2D_ROTATE_0, G2D_FLIP_V, "flip vertical");
	test_rotate(G2D_ROTATE_90, 0, "rotate 90");
	test_rotate(G2D_ROTATE_180, 0, "rotate 180");
	test_rotate(G2D_ROTATE_270, 0, "rotate 270");
	test_yuv(G2D_PIXFMT_NV12, "NV12 -> ARGB8888");
	test_yuv(G2D_PIXFMT_NV21, "NV21 -> ARGB8888");
	test_yuv(G2D_PIXFMT_I420, "I420 -> ARGB8888");
	test_yuv(G2D_PIXFMT_YUYV, "YUYV -> ARGB8888");
	test_blend(G2D_ALPHA_PIXEL, 0xff, "blend src-over, pixel alpha");
	test_blend(G2D_ALPHA_GLOBAL, 0x80, "blend src-over, global alpha");
	test_blend(G2D_ALPHA_MIXED, 0x80, "blend src-over, mixed alpha");

	LOG_I("G2D self test: %s (%d failed)", failures ? "FAIL" : "PASS", failures);
	rt_kprintf("TEST g2d: %s (fail=%d)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? -1 : 0;
}
MSH_CMD_EXPORT(test_g2d, G2D operations against a CPU reference);

/*
 * g2d_bench: 720p operations, hardware time only (from the start of the command list to the end
 * interrupt; no cache maintenance, no queueing, no thread switch). The table runs three times:
 * as the system is set up, with the limit inside the G2D off, and with the G2D also at the highest
 * priority of the memory bus without a bandwidth limit.
 */
#include "mbus-sun252i.h"

#define BENCH_LOOPS 20
#define MBUS_PMU_G2D 9

static rt_uint32_t bench_run(const struct g2d_op *op, int loops, rt_uint32_t *min, rt_uint32_t *max, rt_uint32_t *traffic)
{
    rt_uint32_t i, t, sum = 0, tr0 = mbus_traffic(MBUS_PMU_G2D);

    *min = ~0u;
    *max = 0;
    for (i = 0; i < loops; i++)
    {
        if (g2d_run(op, rt_tick_from_millisecond(2000)) != 0)
            return 0;
        t = g2d_last_hw_time_x100us();
        sum += t;
        *min = t < *min ? t : *min;
        *max = t > *max ? t : *max;
    }
    if (traffic)
        *traffic = (mbus_traffic(MBUS_PMU_G2D) - tr0) / loops;

    return sum / loops;
}

static void bench_report(const char *name, const struct g2d_op *op, rt_uint32_t pix, rt_uint32_t bytes)
{
    rt_uint32_t t, min, max, traffic;

    t = bench_run(op, BENCH_LOOPS, &min, &max, &traffic);
    if (!t)
    {
        rt_kprintf("bench %-28s failed\n", name);
        return;
    }
    /* t is in 1/100 us: Mpix/s = pix * 100 / t, bytes * 100 / t is MB/s (10^6 bytes) */
    rt_kprintf("bench %-26s hw %3u.%02u ms (min %u.%02u max %u.%02u) %4u Mpix/s, %4u MB/s (bus counter %u KB per run)\n", name,
               t / 100000, (t / 1000) % 100, min / 100000, (min / 1000) % 100, max / 100000, (max / 1000) % 100,
               (rt_uint32_t)((rt_uint64_t)pix * 100u / t), (rt_uint32_t)((rt_uint64_t)bytes * 100u / t), traffic / 1024u);
}

static void bench_table(void *a, void *b)
{
    const rt_uint32_t BW = 1280, BH = 720, px = BW * BH;
    struct g2d_rect full = {0, 0, BW, BH};
    struct g2d_op op;

    rt_memset(&op, 0, sizeof(op));
    op.flags = G2D_FLAG_NO_CACHE_OPS;
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = full;

    op.type = G2D_OP_FILL;
    op.color = 0xff336699;
    bench_report("fill ARGB8888", &op, px, px * 4);

    op.type = G2D_OP_BLIT;
    op.src = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {a}, {BW * 4}};
    op.src_rect = full;
    bench_report("copy ARGB8888", &op, px, px * 8);

    op.src = (struct g2d_surface){G2D_PIXFMT_RGB565, BW, BH, {a}, {BW * 2}};
    bench_report("RGB565 -> ARGB8888", &op, px, px * 6);

    op.src = (struct g2d_surface){G2D_PIXFMT_NV12, BW, BH, {a, (rt_uint8_t *)a + BW * BH}, {BW, BW}};
    bench_report("NV12 -> ARGB8888", &op, px, px * 4 + px * 3 / 2);

    op.src = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {a}, {BW * 4}};
    op.src_rect = (struct g2d_rect){0, 0, BW / 2, BH / 2};
    bench_report("scale up 640x360 -> 720p", &op, px, px * 4 + px);

    op.src_rect = full;
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BH, BW, {b}, {BH * 4}};
    op.dst_rect = (struct g2d_rect){0, 0, BH, BW};
    op.rotation = G2D_ROTATE_90;
    bench_report("rotate 90", &op, px, px * 8);
    op.rotation = G2D_ROTATE_0;

    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = full;
    op.flags |= G2D_FLIP_H;
    bench_report("flip horizontal", &op, px, px * 8);
    op.flags &= ~G2D_FLIP_H;

    op.type = G2D_OP_BLEND;
    op.bg = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.bg_rect = full;
    op.blend.mode = G2D_BLEND_SRC_OVER;
    op.blend.fg_alpha_mode = G2D_ALPHA_PIXEL;
    op.blend.bg_alpha_mode = G2D_ALPHA_PIXEL;
    bench_report("blend src-over", &op, px, px * 12);
}

/* find the bus master of the G2D: the one whose bandwidth limit slows a copy down */
static int bench_find_master(void *a, void *b)
{
    const rt_uint32_t BW = 1280, BH = 720;
    struct g2d_op op;
    rt_uint32_t base, t, min, max, m, old;

    rt_memset(&op, 0, sizeof(op));
    op.flags = G2D_FLAG_NO_CACHE_OPS;
    op.type = G2D_OP_BLIT;
    op.src = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {a}, {BW * 4}};
    op.src_rect = (struct g2d_rect){0, 0, BW, BH};
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = op.src_rect;
    base = bench_run(&op, 3, &min, &max, RT_NULL);
    for (m = 0; m < 40; m++)    /* the MBUS has 40 masters */
    {
        if (m == 39)    /* the CPU */
            continue;
        mbus_get_limit(m, &old);
        mbus_set_limit(m, 50);
        t = bench_run(&op, 2, &min, &max, RT_NULL);
        mbus_set_limit(m, old);
        if (t == 0 || t > base * 3u / 2u)    /* 0: the job ran into its timeout */
            return (int)m;
    }

    return -1;
}

static int g2d_bench(int argc, char **argv)
{
    const rt_uint32_t BW = 1280, BH = 720;
    void *a = rt_malloc_align(BW * BH * 4, 64), *b = rt_malloc_align(BW * BH * 4, 64);
    rt_uint32_t old_limit = g2d_get_ddr_limit(), old_prio = 0, old_mbps = 0;
    int master;

    if (!a || !b)
    {
        rt_kprintf("g2d_bench: no memory for two 720p ARGB8888 buffers\n");
        goto out;
    }
    /* the sources are only read and the tests run one after the other: every format reuses the memory of a */
    memset(a, 0x80, BW * BH * 4);
    memset(b, 0x40, BW * BH * 4);

    master = bench_find_master(a, b);
    rt_kprintf("g2d_bench: 720p, G2D limit level %u, bus master of the G2D: %d\n", old_limit, master);

    rt_kprintf("--- as set up\n");
    bench_table(a, b);

    rt_kprintf("--- limit inside the G2D off\n");
    g2d_set_ddr_limit(0);
    bench_table(a, b);

    if (master >= 0)
    {
        mbus_get_priority(master, &old_prio);
        mbus_get_limit(master, &old_mbps);
        mbus_set_priority(master, 3);
        mbus_set_limit(master, 0);
        rt_kprintf("--- and the memory bus: G2D at priority 3, no bandwidth limit\n");
        bench_table(a, b);
        mbus_set_priority(master, old_prio);
        mbus_set_limit(master, old_mbps);
    }
    g2d_set_ddr_limit(old_limit);
out:
    rt_free_align(a);
    rt_free_align(b);

    return 0;
}
MSH_CMD_EXPORT(g2d_bench, 720p G2D operations: hardware time per operation);

/* a fill must reach the memory: the first and last word of the buffer carry the colour */
static rt_bool_t bench_fill_ok(void *b)
{
    const rt_uint32_t BW = 1280, BH = 720;
    struct g2d_op op;
    rt_uint32_t *w = b;

    rt_memset(&op, 0, sizeof(op));
    op.type = G2D_OP_FILL;
    op.color = 0xff12ab34;
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = (struct g2d_rect){0, 0, BW, BH};
    w[0] = w[BW * BH - 1] = 0;
    sunxi_dcache_flush(b, BW * BH * 4);
    if (g2d_run(&op, rt_tick_from_millisecond(2000)) != 0)
        return RT_FALSE;

    return w[0] == 0xff12ab34u && w[BW * BH - 1] == 0xff12ab34u;
}

/* g2d_bench_clk: the 720p table at each module clock, up to the highest the clock source gives */
static int g2d_bench_clk(int argc, char **argv)
{
    static const rt_uint32_t rates[] = {150000000u, 200000000u, 300000000u, 400000000u, 600000000u, 1200000000u};
    const rt_uint32_t BW = 1280, BH = 720;
    void *a = rt_malloc_align(BW * BH * 4, 64), *b = rt_malloc_align(BW * BH * 4, 64);
    rt_uint32_t old = g2d_get_clock_rate(), i, got;

    if (!a || !b)
    {
        rt_kprintf("g2d_bench_clk: no memory\n");
        goto out;
    }
    memset(a, 0x80, BW * BH * 4);
    memset(b, 0x40, BW * BH * 4);
    g2d_set_ddr_limit(0);
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        got = g2d_set_clock_rate(rates[i]);
        rt_kprintf("--- module clock %u MHz requested, %u MHz set, output %s\n", rates[i] / 1000000u, got / 1000000u,
                   bench_fill_ok(b) ? "correct" : "WRONG");
        bench_table(a, b);
    }
out:
    g2d_set_clock_rate(old ? old : 300000000u);
    g2d_set_ddr_limit(0x90);
    rt_free_align(a);
    rt_free_align(b);

    return 0;
}
MSH_CMD_EXPORT(g2d_bench_clk, 720p G2D table at module clocks from 150 MHz to the highest);
