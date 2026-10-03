/* Small helpers shared by the register level code of the G2D and the video engine drivers. */
#ifndef __SUNXI_UTIL_H__
#define __SUNXI_UTIL_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include <errno.h>

#define BIT(n)			(1UL << (n))
#define GENMASK(h, l)		(((~0UL) << (l)) & (~0UL >> (31 - (h))))
#define FIELD_PREP(mask, val)	(((uint32_t)(val) << __builtin_ctz(mask)) & (mask))
#define FIELD_GET(mask, reg)	(((uint32_t)(reg) & (mask)) >> __builtin_ctz(mask))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi)	MIN(MAX((v), (lo)), (hi))
#define ROUND_UP(x, a)		((((x) + (a) - 1) / (a)) * (a))
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(n, d)	(((n) + ((d) / 2)) / (d))
#define IS_ALIGNED(x, a)	(((x) & ((a) - 1)) == 0)
#define ARG_UNUSED(x)		(void)(x)
#define __aligned(n)		__attribute__((aligned(n)))
#define __ASSERT_NO_MSG(c)	RT_ASSERT(c)

#define CONTAINER_OF(p, type, member)	((type *)((char *)(p) - offsetof(type, member)))
#define ROUND_DOWN(x, a)	(((x) / (a)) * (a))
#define IS_ENABLED(x)		(x + 0)

#define BUILD_ASSERT(c, ...) _Static_assert(c, "" __VA_ARGS__)

/* data cache maintenance of a buffer a device reads (flush) or wrote (invalidate) */
static inline void sunxi_dcache_flush(void *addr, size_t size)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, addr, size);
}

static inline void sunxi_dcache_inval(void *addr, size_t size)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, addr, size);
}

static inline void sunxi_dcache_flush_inval(void *addr, size_t size)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH | RT_HW_CACHE_INVALIDATE, addr, size);
}

#endif
