// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - pin control and GPIO helpers for board data.
 */
#define DPY_LOG_TAG "pins"
#include <dpy/dpy_log.h>
#include <dpy/dpy_pdata.h>

void dpy_pins_apply(const struct dpy_pin_group *group)
{
	if (group && group->np && dpy_os_pins_apply(group->np))
		dpy_warn("cannot apply the pin state\n");
}

void dpy_pins_release(const struct dpy_pin_group *group)
{
	if (group && group->np)
		dpy_os_pins_release(group->np);
}

int dpy_gpio_set(const struct dpy_gpio *gpio, bool on)
{
	int level;

	if (!gpio || !(gpio->flags & DPY_GPIO_VALID))
		return 0;
	level = on ? 1 : 0;
	if (gpio->flags & DPY_GPIO_ACTIVE_LOW)
		level = !level;
	return dpy_os_gpio_direction_output(gpio->pin, level);
}
