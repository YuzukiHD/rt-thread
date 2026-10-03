// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline description built from the device tree at run time.
 *
 * Every available node of the display pipeline (display engine, TCON top and
 * LCD, RGB/LVDS/DSI encoders, combo D-PHY, panels, backlights) becomes a
 * struct dpy_node of the graph the display core consumes:
 *
 *   reg / interrupts / clocks / resets  ->  struct dpy_res
 *   top / phy / backlight phandles      ->  struct dpy_ref
 *   ports/port@N/endpoint@M             ->  struct dpy_link (source side)
 *   the remaining properties            ->  the pdata of the matched driver
 *
 * Command sequences of the panels (power, init) are cell arrays in the device
 * tree and are decoded here into struct dpy_cmd lists.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <errno.h>

#include <dpy/dpy_graph.h>
#include <dpy/dpy_os.h>
#include <dpy/dpy_pdata.h>
#include <hal/display/display_engine.h>

#include "dt_graph.h"

#define MAX_RES		12
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

struct dpy_board_desc dpy_board;

static struct dpy_node nodes[DPY_GRAPH_MAX_NODES];
static struct dpy_link links[DPY_GRAPH_MAX_LINKS];
static unsigned int nnodes, nlinks;
static rt_bool_t built;

static rt_uint32_t prop_u32(struct rt_ofw_node *np, const char *name, rt_uint32_t def)
{
	rt_uint32_t v;

	return rt_ofw_prop_read_u32(np, name, &v) ? def : v;
}

/* string @index of a list property; false past the end (the read returns 0 strings there) */
static rt_bool_t prop_str(struct rt_ofw_node *np, const char *name, int index, const char **out)
{
	return rt_ofw_prop_read_string_array_index(np, name, index, 1, out) > 0;
}

static int prop_enum(struct rt_ofw_node *np, const char *name, const char *const *list, int count)
{
	const char *s;
	int i;

	if (!prop_str(np, name, 0, &s))
		return 0;
	for (i = 0; i < count; i++)
		if (!strcmp(s, list[i]))
			return i;
	return 0;
}

/* a <&pio bank number flags> property of @np as a global pin and its polarity */
static void prop_gpio(struct rt_ofw_node *np, const char *name, struct dpy_gpio *g)
{
	char prop[48];
	rt_ssize_t pin;
	rt_uint32_t flags = 0;

	memset(g, 0, sizeof(*g));
	rt_snprintf(prop, sizeof(prop), "%s-gpios", name);
	if (!rt_ofw_prop_read_raw(np, prop, RT_NULL))
		return;
	pin = rt_ofw_get_named_pin(np, name, 0, RT_NULL, RT_NULL);
	if (pin < 0)
		return;
	rt_ofw_prop_read_u32_index(np, prop, 3, &flags);
	g->pin = pin;
	g->flags = DPY_GPIO_VALID | ((flags & 1U) ? DPY_GPIO_ACTIVE_LOW : 0);
}

static void add_res(struct dpy_res *res, unsigned int *n, struct dpy_res r)
{
	if (*n < MAX_RES)
		res[(*n)++] = r;
}

static const struct dpy_res *build_res(struct rt_ofw_node *np, unsigned int *count)
{
	struct dpy_res *res = rt_calloc(MAX_RES, sizeof(*res));
	rt_uint64_t addr, size;
	const char *cname;
	unsigned int n = 0;
	int i;

	if (!res)
		return RT_NULL;

	if (!rt_ofw_get_address(np, 0, &addr, &size)) {
		struct dpy_res r = DPY_RES_MMIO("reg", dpy_os_ioremap((uintptr_t)addr, (size_t)size), (uint32_t)size);

		add_res(res, &n, r);
	}
	if (rt_ofw_get_irq_count(np) > 0) {
		struct dpy_res r = DPY_RES_IRQ("irq", rt_ofw_get_irq(np, 0));

		add_res(res, &n, r);
	}
	for (i = 0; prop_str(np, "clock-names", i, &cname); i++) {
		struct dpy_res r = DPY_RES_CLK(cname, rt_ofw_get_clk(np, i), 0);
		rt_uint32_t rate = 0;

		rt_ofw_prop_read_u32_index(np, "clock-rates", i, &rate);
		r.clk.rate = rate;
		add_res(res, &n, r);
	}
	for (i = 0; prop_str(np, "reset-names", i, &cname); i++) {
		struct dpy_res r = DPY_RES_RST(cname, rt_ofw_get_reset_control_by_index(np, i));

		add_res(res, &n, r);
	}
	*count = n;
	return res;
}

static const struct dpy_ref *build_refs(struct rt_ofw_node *np, unsigned int *count)
{
	static const char *const names[] = { "top", "phy", "backlight" };
	struct dpy_ref *refs = rt_calloc(ARRAY_SIZE(names), sizeof(*refs));
	unsigned int i, n = 0;

	if (!refs)
		return RT_NULL;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct rt_ofw_node *target = rt_ofw_parse_phandle(np, names[i], 0);

		if (!target)
			continue;
		refs[n].name = names[i];
		refs[n].target = rt_ofw_node_full_name(target);
		n++;
	}
	*count = n;
	if (!n) {
		rt_free(refs);
		return RT_NULL;
	}
	return refs;
}

static struct dpy_node *add_node(struct rt_ofw_node *np, const char *const *compatible, const void *pdata)
{
	struct dpy_node *n;

	if (nnodes >= DPY_GRAPH_MAX_NODES)
		return RT_NULL;
	n = &nodes[nnodes++];
	n->name = rt_ofw_node_full_name(np);
	n->compatible = compatible;
	n->status = DPY_STATUS_OKAY;
	n->pdata = pdata;
	{
		unsigned int c = 0;

		n->res = build_res(np, &c);
		n->nres = c;
		n->refs = build_refs(np, &c);
		n->nrefs = c;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Command sequences                                                   */
/* ------------------------------------------------------------------ */
static size_t seq_data_len(uint32_t type, uint32_t len)
{
	switch (type) {
	case DPY_CMD_DCS:
	case DPY_CMD_GENERIC:
	case DPY_CMD_SPI_CMD:
	case DPY_CMD_SPI_DATA:
		return len;
	default:
		return 0;
	}
}

static int seq_decode(struct rt_ofw_node *np, const char *prop, struct dpy_cmd_seq *dst)
{
	int n = rt_ofw_prop_count_of_size(np, prop, sizeof(rt_uint32_t));
	rt_uint32_t *c;
	size_t i, count = 0, bytes = 0, k = 0;
	struct dpy_cmd *cmds;
	uint8_t *data;

	if (n <= 0)
		return 0;
	c = rt_malloc(n * sizeof(*c));
	if (!c)
		return -ENOMEM;
	rt_ofw_prop_read_u32_array_index(np, prop, 0, n, c);

	for (i = 0; i + 3 <= (size_t)n; count++) {
		size_t dl = seq_data_len(c[i], c[i + 1]);

		if (i + 3 + dl > (size_t)n) {
			rt_free(c);
			return -EINVAL;
		}
		bytes += dl;
		i += 3 + dl;
	}
	if (i != (size_t)n) {
		rt_free(c);
		return -EINVAL;
	}
	cmds = dpy_os_zalloc(count * sizeof(*cmds));
	data = dpy_os_zalloc(bytes ? bytes : 1);
	if (!cmds || !data) {
		dpy_os_free(cmds);
		dpy_os_free(data);
		rt_free(c);
		return -ENOMEM;
	}
	for (i = 0, count = 0; i < (size_t)n; count++) {
		size_t dl = seq_data_len(c[i], c[i + 1]), j;

		cmds[count].type = c[i];
		cmds[count].len = c[i + 1];
		cmds[count].arg = c[i + 2];
		if (dl) {
			cmds[count].data = &data[k];
			for (j = 0; j < dl; j++)
				data[k++] = c[i + 3 + j];
		}
		i += 3 + dl;
	}
	dst->cmds = cmds;
	dst->count = count;
	rt_free(c);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Panels                                                              */
/* ------------------------------------------------------------------ */
static struct dpy_display_mode *panel_modes(struct rt_ofw_node *np, uint8_t *count, uint32_t *bus_flags)
{
	struct rt_ofw_node *timings = rt_ofw_get_child_by_tag(np, "display-timings");
	struct rt_ofw_node *t;
	struct dpy_display_mode *modes;
	rt_uint32_t w = prop_u32(np, "width", 0), h = prop_u32(np, "height", 0);
	unsigned int n = 0;

	*count = 0;
	*bus_flags = 0;
	if (!timings)
		return RT_NULL;
	modes = rt_calloc(8, sizeof(*modes));
	if (!modes)
		return RT_NULL;
	rt_ofw_foreach_available_child_node(timings, t) {
		struct dpy_display_mode *m;
		rt_uint32_t hfp = prop_u32(t, "hfront-porch", 0), hsl = prop_u32(t, "hsync-len", 0);
		rt_uint32_t hbp = prop_u32(t, "hback-porch", 0), vfp = prop_u32(t, "vfront-porch", 0);
		rt_uint32_t vsl = prop_u32(t, "vsync-len", 0), vbp = prop_u32(t, "vback-porch", 0);

		if (n >= 8)
			break;
		m = &modes[n++];
		m->clock = prop_u32(t, "clock-frequency", 0) / 1000;
		m->hdisplay = w;
		m->hsync_start = w + hfp;
		m->hsync_end = w + hfp + hsl;
		m->htotal = w + hfp + hsl + hbp;
		m->vdisplay = h;
		m->vsync_start = h + vfp;
		m->vsync_end = h + vfp + vsl;
		m->vtotal = h + vfp + vsl + vbp;
		m->flags = (prop_u32(t, "hsync-active", 0) ? 0 : DISPLAY_MODE_FLAG_NHSYNC) |
			   (prop_u32(t, "vsync-active", 0) ? 0 : DISPLAY_MODE_FLAG_NVSYNC) |
			   DISPLAY_MODE_FLAG_PREFERRED;
		*bus_flags = (prop_u32(t, "de-active", 1) ? 0 : DPY_BUS_FLAG_DE_LOW) |
			     (prop_u32(t, "pixelclk-active", 1) ? 0 : DPY_BUS_FLAG_PIXDATA_NEGEDGE);
	}
	*count = n;
	return modes;
}

static void add_panel_simple(struct rt_ofw_node *np)
{
	static const char *const bus[] = { "rgb888-1x24", "rgb666-1x18", "rgb565-1x16",
					   "rgb666-1x7x3-spwg", "rgb888-1x7x4-spwg", "rgb888-1x7x4-jeida" };
	static const char *const spi[] = { "none", "3wire-9bit", "4wire-8bit" };
	static const char *const compat[] = { "panel-simple", RT_NULL };
	struct dpy_panel_simple_pdata *pd = rt_calloc(1, sizeof(*pd));

	if (!pd)
		return;
	pd->name = rt_ofw_node_full_name(np);
	pd->modes = panel_modes(np, &pd->num_modes, &pd->bus_flags);
	pd->width_mm = prop_u32(np, "width-mm", 0);
	pd->height_mm = prop_u32(np, "height-mm", 0);
	pd->enable_delay_ms = prop_u32(np, "enable-delay-ms", 0);
	pd->disable_delay_ms = prop_u32(np, "disable-delay-ms", 0);
	pd->bus_format = prop_enum(np, "bus-format", bus, ARRAY_SIZE(bus)) + 1;
	pd->spi.mode = prop_enum(np, "spi-mode", spi, ARRAY_SIZE(spi));
	prop_gpio(np, "spi-cs", &pd->spi.cs);
	prop_gpio(np, "spi-sck", &pd->spi.sck);
	prop_gpio(np, "spi-sda", &pd->spi.sda);
	prop_gpio(np, "spi-dc", &pd->spi.dc);
	pd->spi.half_period_us = prop_u32(np, "spi-half-period-us", 1);
	seq_decode(np, "power-on-sequence", &pd->power_on);
	seq_decode(np, "power-off-sequence", &pd->power_off);
	seq_decode(np, "init-sequence", &pd->init);
	seq_decode(np, "exit-sequence", &pd->exit);
	add_node(np, compat, pd);
}

static void add_panel_dsi(struct rt_ofw_node *np)
{
	static const char *const fmt[] = { "rgb888", "rgb666", "rgb666-packed", "rgb565" };
	static const char *const compat[] = { "panel-dsi", RT_NULL };
	struct dpy_panel_dsi_pdata *pd = rt_calloc(1, sizeof(*pd));
	uint32_t unused;

	if (!pd)
		return;
	pd->name = rt_ofw_node_full_name(np);
	pd->modes = panel_modes(np, &pd->num_modes, &unused);
	pd->width_mm = prop_u32(np, "width-mm", 0);
	pd->height_mm = prop_u32(np, "height-mm", 0);
	pd->enable_delay_ms = prop_u32(np, "enable-delay-ms", 0);
	pd->disable_delay_ms = prop_u32(np, "disable-delay-ms", 0);
	pd->lanes = rt_ofw_prop_count_of_size(np, "data-lanes", sizeof(rt_uint32_t));
	pd->format = prop_enum(np, "mipi-dsi-format", fmt, ARRAY_SIZE(fmt));
	pd->mode_flags = prop_u32(np, "mode-flags", 0);
	pd->hs_trail = prop_u32(np, "hs-trail", 0);
	pd->clk_trail = prop_u32(np, "clk-trail", 0);
	seq_decode(np, "power-on-sequence", &pd->power_on);
	seq_decode(np, "power-off-sequence", &pd->power_off);
	seq_decode(np, "init-sequence", &pd->init);
	seq_decode(np, "exit-sequence", &pd->exit);
	add_node(np, compat, pd);
}

/* ------------------------------------------------------------------ */
/* Backlights                                                          */
/* ------------------------------------------------------------------ */
static void add_backlight_pwm(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "pwm-backlight", RT_NULL };
	struct dpy_backlight_pwm_pdata *pd = rt_calloc(1, sizeof(*pd));
	rt_uint32_t v;

	if (!pd)
		return;
	pd->controller = rt_ofw_parse_phandle(np, "pwms", 0);
	if (!rt_ofw_prop_read_u32_index(np, "pwms", 1, &v))
		pd->channel = v;
	if (!rt_ofw_prop_read_u32_index(np, "pwms", 2, &v))
		pd->period_ns = v;
	if (!rt_ofw_prop_read_u32_index(np, "pwms", 3, &v))
		pd->inverted = !!(v & 1U);
	prop_gpio(np, "enable", &pd->enable);
	pd->max_level = prop_u32(np, "max-level", 255);
	pd->default_level = prop_u32(np, "default-level", 255);
	pd->min_level = prop_u32(np, "min-level", 0);
	add_node(np, compat, pd);
}

static void add_backlight_gpio(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "gpio-backlight", RT_NULL };
	struct dpy_backlight_gpio_pdata *pd = rt_calloc(1, sizeof(*pd));

	if (!pd)
		return;
	prop_gpio(np, "enable", &pd->enable);
	add_node(np, compat, pd);
}

/* ------------------------------------------------------------------ */
/* Engine and encoders                                                 */
/* ------------------------------------------------------------------ */
static struct dpy_pin_group pins_of(struct rt_ofw_node *np)
{
	struct dpy_pin_group g = { .np = rt_ofw_prop_read_raw(np, "pinctrl-0", RT_NULL) ? np : RT_NULL };

	return g;
}

static void add_de(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "allwinner,sun252iw2-display-engine", RT_NULL };
	struct dpy_engine_pdata *pd = rt_calloc(1, sizeof(*pd));

	if (!pd)
		return;
	pd->adjust.brightness = prop_u32(np, "brightness", 50);
	pd->adjust.contrast = prop_u32(np, "contrast", 50);
	pd->adjust.saturation = prop_u32(np, "saturation", 50);
	pd->adjust.hue = prop_u32(np, "hue", 50);
	pd->background = prop_u32(np, "background-color", 0);
	add_node(np, compat, pd);
}

static void add_rgb(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "allwinner,sunxi-rgb", RT_NULL };
	struct dpy_rgb_pdata *pd = rt_calloc(1, sizeof(*pd));

	if (!pd)
		return;
	pd->pins = pins_of(np);
	pd->hv_mode = prop_u32(np, "hv-mode", 0);
	pd->srgb_seq = prop_u32(np, "serial-rgb-sequence", 0);
	pd->syuv_seq = prop_u32(np, "serial-yuv-sequence", 0);
	pd->syuv_fdly = prop_u32(np, "serial-yuv-first-delay", 0);
	pd->rgb_swap = prop_u32(np, "rgb-swap", 0);
	pd->rb_swap = prop_u32(np, "rb-swap", 0);
	pd->clk_phase = prop_u32(np, "clk-phase", 0);
	pd->use_tcon_frm = rt_ofw_prop_read_bool(np, "use-tcon-frm");
	pd->io_adjust = prop_u32(np, "io-adjust", 0);
	add_node(np, compat, pd);
}

static void add_lvds(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "allwinner,sunxi-lvds", RT_NULL };
	struct dpy_lvds_pdata *pd = rt_calloc(1, sizeof(*pd));

	if (!pd)
		return;
	pd->pins = pins_of(np);
	pd->dual_link = rt_ofw_prop_read_bool(np, "dual-link");
	pd->use_tcon_frm = rt_ofw_prop_read_bool(np, "use-tcon-frm");
	add_node(np, compat, pd);
}

static void add_dsi(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "allwinner,sun252iw2-mipi-dsi", "allwinner,sunxi-mipi-dsi", RT_NULL };
	struct dpy_dsi_pdata *pd = rt_calloc(1, sizeof(*pd));

	if (!pd)
		return;
	pd->pins = pins_of(np);
	add_node(np, compat, pd);
}

/* ------------------------------------------------------------------ */
/* Links: ports/port@N/endpoint@M { remote-endpoint = <&sink>; }       */
/* ------------------------------------------------------------------ */
static void add_links(struct rt_ofw_node *np)
{
	struct rt_ofw_node *ports = rt_ofw_get_child_by_tag(np, "ports");
	struct rt_ofw_node *port, *ep;

	if (!ports)
		return;
	rt_ofw_foreach_child_node(ports, port) {
		rt_ofw_foreach_child_node(port, ep) {
			struct rt_ofw_node *sink = rt_ofw_parse_phandle(ep, "remote-endpoint", 0);

			if (!sink || !rt_ofw_node_is_available(sink) || nlinks >= DPY_GRAPH_MAX_LINKS)
				continue;
			links[nlinks].a = rt_ofw_node_full_name(np);
			links[nlinks].a_port = prop_u32(port, "reg", 0);
			links[nlinks].a_ep = prop_u32(ep, "reg", 0);
			links[nlinks].b = rt_ofw_node_full_name(sink);
			links[nlinks].b_port = prop_u32(ep, "remote-port", 0);
			links[nlinks].b_ep = prop_u32(ep, "remote-ep", 0);
			nlinks++;
		}
	}
}

#define FOREACH_NODE(compat, fn)					\
	do {								\
		struct rt_ofw_node *np_;				\
		rt_ofw_foreach_node_by_compatible(np_, compat)		\
			if (rt_ofw_node_is_available(np_))		\
				fn(np_);				\
	} while (0)

static void add_plain(struct rt_ofw_node *np, const char *const *compat, const void *pdata)
{
	add_node(np, compat, pdata);
}

static void add_tcon_top(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "allwinner,sun252iw2-tcon-top", "allwinner,sunxi-tcon-top", RT_NULL };

	add_plain(np, compat, RT_NULL);
}

static void add_tcon_lcd(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "allwinner,sun252iw2-tcon-lcd", "allwinner,sunxi-tcon-lcd", RT_NULL };

	add_plain(np, compat, RT_NULL);
}

static void add_dphy(struct rt_ofw_node *np)
{
	static const char *const compat[] = { "allwinner,sun252iw2-combo-dphy", "allwinner,sunxi-combo-dphy", RT_NULL };

	add_plain(np, compat, &dpy_sun252iw2_dphy_pdata);
}

static void links_de(struct rt_ofw_node *np) { add_links(np); }

int dpy_ofw_prepare(void)
{
	if (built)
		return 0;
	built = RT_TRUE;

	FOREACH_NODE("allwinner,sun252iw2-display-engine", add_de);
	FOREACH_NODE("allwinner,sun252iw2-tcon-top", add_tcon_top);
	FOREACH_NODE("allwinner,sun252iw2-tcon-lcd", add_tcon_lcd);
	FOREACH_NODE("allwinner,sunxi-rgb", add_rgb);
	FOREACH_NODE("allwinner,sunxi-lvds", add_lvds);
	FOREACH_NODE("allwinner,sun252iw2-mipi-dsi", add_dsi);
	FOREACH_NODE("allwinner,sun252iw2-combo-dphy", add_dphy);
	FOREACH_NODE("panel-simple", add_panel_simple);
	FOREACH_NODE("panel-dsi", add_panel_dsi);
	FOREACH_NODE("pwm-backlight", add_backlight_pwm);
	FOREACH_NODE("gpio-backlight", add_backlight_gpio);

	FOREACH_NODE("allwinner,sun252iw2-display-engine", links_de);
	FOREACH_NODE("allwinner,sun252iw2-tcon-lcd", links_de);
	FOREACH_NODE("allwinner,sunxi-rgb", links_de);
	FOREACH_NODE("allwinner,sunxi-lvds", links_de);
	FOREACH_NODE("allwinner,sun252iw2-mipi-dsi", links_de);

	dpy_board.name = "devicetree";
	dpy_board.nodes = nodes;
	dpy_board.nnodes = nnodes;
	dpy_board.links = links;
	dpy_board.nlinks = nlinks;
	return nnodes ? 0 : -ENODEV;
}
