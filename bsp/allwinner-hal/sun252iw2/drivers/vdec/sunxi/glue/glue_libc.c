/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* C library functions the archive expects that the minimal libc does not have */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <rtthread.h>
#include <sunxi-util.h>

#include "glue.h"

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
	rt_kprintf("assert: %s:%d %s: %s\n", file, line, func, expr);
	for (;;) {
	}
}

int usleep(unsigned int us)
{
	/* the archive polls with short sleeps: below two ticks a tick based sleep is far too coarse */
	if (us >= 2000000U / RT_TICK_PER_SECOND)
		rt_thread_mdelay(us / 1000U);
	else
		rt_hw_us_delay(us);
	return 0;
}

/* The archive can dump data to files for debugging; there is no file system here. */
__attribute__((weak)) FILE *fopen(const char *path, const char *mode)
{
	(void)path;
	(void)mode;
	return NULL;
}

__attribute__((weak)) int fclose(FILE *f)
{
	(void)f;
	return -1;
}

__attribute__((weak)) int open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	return -1;
}

__attribute__((weak)) int close(int fd)
{
	(void)fd;
	return -1;
}

__attribute__((weak)) int ioctl(int fd, unsigned long req, ...)
{
	(void)fd;
	(void)req;
	return -1;
}

/*
 * The archive prints its messages with printf() (the build wraps that symbol).
 * They are turned into log messages of the glue module: the level comes from the word the line starts with, the colour codes
 * are removed.
 */
#define ARCHIVE_LINE_MAX 256

static void strip_ansi(char *s)
{
	char *out = s;

	while (*s != '\0') {
		if (s[0] == '\033' && s[1] == '[') {
			s += 2;
			while (*s != '\0' && *s != 'm') {
				s++;
			}
			if (*s == 'm') {
				s++;
			}
			continue;
		}
		*out++ = *s++;
	}
	*out = '\0';
}

int __wrap_printf(const char *fmt, ...)
{
	char line[ARCHIVE_LINE_MAX];
	char *text;
	va_list ap;
	size_t len;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	strip_ansi(line);
	len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
		line[--len] = '\0';
	}
	if (len == 0) {
		return n;
	}

	/* "level  : text": the level word is replaced by the log level itself */
	text = line;
	if (strncmp(line, "error", 5) == 0 || strncmp(line, "warn", 4) == 0 ||
	    strncmp(line, "info", 4) == 0) {
		char *colon = strstr(line, ": ");

		if (colon != NULL) {
			text = colon + 2;
		}
	}

	if (strncmp(line, "error", 5) == 0) {
		LOG_ERR("%s", text);
	} else if (strncmp(line, "warn", 4) == 0) {
		LOG_WRN("%s", text);
	} else if (strncmp(line, "info", 4) == 0) {
		LOG_INF("%s", text);
	} else {
		LOG_DBG("%s", text);
	}

	return n;
}
