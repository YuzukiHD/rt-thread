// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - logging through the RT-Thread console.
 */
#include <rtthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dpy/dpy_log.h>

/* messages above this level are dropped */
int dpy_log_level = DPY_LOG_INFO;

void dpy_log(int level, const char *tag, const char *fmt, ...)
{
	static const char *const names[] = { "E", "W", "I", "D" };
	char msg[192];
	va_list ap;
	size_t len;

	if (level > dpy_log_level)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	len = strlen(msg);
	while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
		msg[--len] = '\0';
	rt_kprintf("[display %s] %s: %s\n", names[level & 3], tag, msg);
}
