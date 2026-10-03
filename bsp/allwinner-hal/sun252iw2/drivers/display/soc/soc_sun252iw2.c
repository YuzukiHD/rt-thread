// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * sun252iw2 display subsystem: the code that cannot be described in
 * the devicetree. The hardware graph itself (register windows, clocks,
 * resets, IRQs, links) lives in the devicetree, see ofw_graph.c.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <errno.h>
#include <dpy/dpy_graph.h>
#include <dpy/dpy_os.h>
#include <dpy/dpy_pdata.h>

#include "../dt_graph.h"

/* the system configuration and the chip id (fuse) blocks of the device tree */
static uintptr_t node_regs(const char *compatible)
{
	struct rt_ofw_node *np = rt_ofw_find_node_by_compatible(RT_NULL, compatible);

	return np ? (uintptr_t)rt_ofw_iomap(np, 0) : 0;
}

/*
 * The DSI/LVDS pad termination is trimmed per chip: a 4 bit code is fused
 * in SID word 0x18 bits [7:4] and has to be copied, together with an
 * unlock key, into the SYS_CFG resistor control register.
 */
static void sun252iw2_dphy_calibrate(void)
{
	uintptr_t sid = node_regs("allwinner,sun252i-sid");
	uintptr_t syscfg = node_regs("allwinner,sun252i-syscon");
	uint32_t code;

	if (!sid || !syscfg)
		return;
	code = dpy_readl(sid + 0x18);
	if (!code)
		return;
	code = (code >> 4) & 0xf;
	dpy_writel(code | (code << 8) | (0x1937U << 16), syscfg + 0x164);
}

const struct dpy_combo_dphy_soc_pdata dpy_sun252iw2_dphy_pdata = {
	.calibrate = sun252iw2_dphy_calibrate,
};

/*
 * The MBUS priority of the display engine is set by the mbus driver
 * from the device tree.
 *
 * SYS_CFG word 0x04 is programmed with 0x0b000000 before
 * the display engine is used (it most likely routes an SRAM area to the
 * display engine). Its meaning is not documented; the value is kept as is.
 */
static int sun252iw2_display_init(void)
{
	uintptr_t syscfg = node_regs("allwinner,sun252i-syscon");

	if (!syscfg)
		return -ENODEV;
	dpy_writel(0x0b000000, syscfg + 0x04);
	return 0;
}

const struct dpy_soc_desc dpy_soc = {
	.name = "sun252iw2",
	.init = sun252iw2_display_init,
};
