// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - OS port for the RT-Thread device model.
 *
 * Implements every function of include/dpy/dpy_os.h on top of the RT-Thread
 * frameworks: clocks and resets through the clk and reset controllers, pins
 * through pinctrl and the pin device, interrupts through the PIC, the
 * backlight through the PWM device.
 */
#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <dpy/dpy_os.h>

#define ARG_UNUSED(x)		(void)(x)
#define ROUND_UP(x, a)		(((x) + ((a) - 1)) & ~((a) - 1))

#define DPY_CACHE_LINE		64

void *rt_ioremap(void *paddr, size_t size);

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
void *dpy_os_zalloc(size_t size)
{
	void *p = rt_malloc(size);

	if (p)
		memset(p, 0, size);
	return p;
}

void dpy_os_free(void *ptr)
{
	if (ptr)
		rt_free(ptr);
}

void *dpy_os_dma_alloc(size_t size, size_t align, dpy_dma_addr_t *dma)
{
	void *p;

	if (align < DPY_CACHE_LINE)
		align = DPY_CACHE_LINE;
	size = ROUND_UP(size, DPY_CACHE_LINE);
	p = rt_malloc_align(size, align);
	if (!p)
		return NULL;
	memset(p, 0, size);
	rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, p, size);
	if (dma)
		*dma = (dpy_dma_addr_t)(uintptr_t)p;
	return p;
}

void dpy_os_dma_free(void *ptr)
{
	if (ptr)
		rt_free_align(ptr);
}

dpy_dma_addr_t dpy_os_virt_to_dma(const void *ptr)
{
	/* the MMU is off: virtual == physical */
	return (dpy_dma_addr_t)(uintptr_t)ptr;
}

void dpy_os_dcache_clean(const void *ptr, size_t size)
{
	uintptr_t s = (uintptr_t)ptr & ~(uintptr_t)(DPY_CACHE_LINE - 1);
	uintptr_t e = ROUND_UP((uintptr_t)ptr + size, DPY_CACHE_LINE);

	if (size)
		rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)s, e - s);
}

void dpy_os_dcache_invalidate(void *ptr, size_t size)
{
	uintptr_t s = (uintptr_t)ptr & ~(uintptr_t)(DPY_CACHE_LINE - 1);
	uintptr_t e = ROUND_UP((uintptr_t)ptr + size, DPY_CACHE_LINE);

	if (size)
		rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)s, e - s);
}

uintptr_t dpy_os_ioremap(uintptr_t phys, size_t size)
{
	return (uintptr_t)rt_ioremap((void *)phys, size);
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
bool dpy_os_in_irq(void)
{
	return rt_interrupt_get_nest() != 0;
}

void dpy_os_udelay(uint32_t us)
{
	rt_hw_us_delay(us);
}

void dpy_os_msleep(uint32_t ms)
{
	if (dpy_os_in_irq() || rt_thread_self() == RT_NULL) {
		rt_hw_us_delay(ms * 1000U);
		return;
	}
	rt_thread_mdelay(ms);
}

uint64_t dpy_os_time_us(void)
{
	return (uint64_t)rt_tick_get_millisecond() * 1000U;
}

/* ------------------------------------------------------------------ */
/* Synchronisation                                                     */
/* ------------------------------------------------------------------ */
struct dpy_mutex {
	struct rt_mutex m;
};

struct dpy_sem {
	struct rt_semaphore s;
};

struct dpy_mutex *dpy_os_mutex_create(void)
{
	struct dpy_mutex *m = rt_malloc(sizeof(*m));

	if (m)
		rt_mutex_init(&m->m, "dpy", RT_IPC_FLAG_PRIO);
	return m;
}

void dpy_os_mutex_destroy(struct dpy_mutex *m)
{
	if (m) {
		rt_mutex_detach(&m->m);
		rt_free(m);
	}
}

void dpy_os_mutex_lock(struct dpy_mutex *m)
{
	rt_mutex_take(&m->m, RT_WAITING_FOREVER);
}

void dpy_os_mutex_unlock(struct dpy_mutex *m)
{
	rt_mutex_release(&m->m);
}

struct dpy_sem *dpy_os_sem_create(uint32_t initial)
{
	struct dpy_sem *s = rt_malloc(sizeof(*s));

	if (s)
		rt_sem_init(&s->s, "dpy", initial, RT_IPC_FLAG_FIFO);
	return s;
}

void dpy_os_sem_destroy(struct dpy_sem *s)
{
	if (s) {
		rt_sem_detach(&s->s);
		rt_free(s);
	}
}

void dpy_os_sem_post(struct dpy_sem *s)
{
	rt_sem_release(&s->s);
}

int dpy_os_sem_wait(struct dpy_sem *s, uint32_t timeout_ms)
{
	rt_int32_t to;

	if (timeout_ms == DPY_WAIT_FOREVER)
		to = RT_WAITING_FOREVER;
	else if (!timeout_ms)
		to = 0;
	else
		to = rt_tick_from_millisecond(timeout_ms);
	return rt_sem_take(&s->s, to) == RT_EOK ? 0 : -ETIMEDOUT;
}

void dpy_os_spin_init(dpy_spinlock_t *lock)
{
	memset(lock, 0, sizeof(*lock));
}

unsigned long dpy_os_spin_lock_irqsave(dpy_spinlock_t *lock)
{
	(void)lock;
	return (unsigned long)rt_hw_interrupt_disable();
}

void dpy_os_spin_unlock_irqrestore(dpy_spinlock_t *lock, unsigned long flags)
{
	(void)lock;
	rt_hw_interrupt_enable((rt_base_t)flags);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */
struct dpy_irq {
	dpy_irq_handler_t handler;
	void *data;
};

static void dpy_os_irq_trampoline(int vector, void *param)
{
	struct dpy_irq *i = param;

	(void)vector;
	i->handler(i->data);
}

int dpy_os_request_irq(int irq, dpy_irq_handler_t handler, const char *name,
		       void *data)
{
	struct dpy_irq *i = dpy_os_zalloc(sizeof(*i));

	if (!i)
		return -ENOMEM;
	i->handler = handler;
	i->data = data;
	if (rt_pic_attach_irq(irq, dpy_os_irq_trampoline, i, name, 0)) {
		dpy_os_free(i);
		return -EINVAL;
	}
	rt_pic_irq_unmask(irq);
	return 0;
}

void dpy_os_free_irq(int irq, void *data)
{
	(void)data;
	rt_pic_irq_mask(irq);
	rt_pic_detach_irq(irq, RT_NULL);
}

/* ------------------------------------------------------------------ */
/* Clocks and resets                                                   */
/* ------------------------------------------------------------------ */
void dpy_os_clk_put(struct dpy_clk *clk)
{
	ARG_UNUSED(clk);
}

int dpy_os_clk_enable(struct dpy_clk *clk)
{
	return clk ? -rt_clk_prepare_enable((struct rt_clk *)clk) : 0;
}

void dpy_os_clk_disable(struct dpy_clk *clk)
{
	if (clk)
		rt_clk_disable_unprepare((struct rt_clk *)clk);
}

int dpy_os_clk_set_rate(struct dpy_clk *clk, uint32_t hz)
{
	return clk ? -rt_clk_set_rate((struct rt_clk *)clk, hz) : -ENODEV;
}

uint32_t dpy_os_clk_get_rate(struct dpy_clk *clk)
{
	return clk ? (uint32_t)rt_clk_get_rate((struct rt_clk *)clk) : 0;
}

uint32_t dpy_os_clk_round_rate(struct dpy_clk *clk, uint32_t hz)
{
	rt_base_t r = clk ? rt_clk_round_rate((struct rt_clk *)clk, hz) : 0;

	return r > 0 ? (uint32_t)r : 0;
}

void dpy_os_reset_put(struct dpy_reset *rst)
{
	ARG_UNUSED(rst);
}

int dpy_os_reset_assert(struct dpy_reset *rst)
{
	return rst ? -rt_reset_control_assert((struct rt_reset_control *)rst) : 0;
}

int dpy_os_reset_deassert(struct dpy_reset *rst)
{
	return rst ? -rt_reset_control_deassert((struct rt_reset_control *)rst) : 0;
}

/* ------------------------------------------------------------------ */
/* Pins, GPIO, PWM, regulators                                          */
/* ------------------------------------------------------------------ */
int dpy_os_pins_apply(const void *np)
{
	struct rt_device dev;

	memset(&dev, 0, sizeof(dev));
	dev.ofw_node = (struct rt_ofw_node *)np;
	return -rt_pin_ctrl_confs_apply_by_name(&dev, "default");
}

void dpy_os_pins_release(const void *np)
{
	rt_ssize_t i = 0;
	struct rt_ofw_node *grp, *child;
	struct rt_ofw_node *node = (struct rt_ofw_node *)np;

	/* park the pads as inputs so the panel sees no stray levels */
	while ((grp = rt_ofw_parse_phandle(node, "pinctrl-0", i++))) {
		rt_ofw_foreach_child_node(grp, child) {
			rt_uint32_t v;
			int k = 0;

			while (!rt_ofw_prop_read_u32_index(child, "pinmux", k++, &v))
				rt_pin_mode(v >> 4, PIN_MODE_INPUT);
		}
		rt_ofw_node_put(grp);
	}
}

int dpy_os_gpio_set_value(uint32_t pin, int value)
{
	rt_pin_write(pin, value ? PIN_HIGH : PIN_LOW);
	return 0;
}

int dpy_os_gpio_direction_output(uint32_t pin, int value)
{
	/* the level is in the data register before the pin becomes an output */
	dpy_os_gpio_set_value(pin, value);
	rt_pin_mode(pin, PIN_MODE_OUTPUT);
	return 0;
}

int dpy_os_gpio_direction_input(uint32_t pin)
{
	rt_pin_mode(pin, PIN_MODE_INPUT);
	return 0;
}

int dpy_os_gpio_get_value(uint32_t pin)
{
	return rt_pin_read(pin) == PIN_HIGH;
}

int dpy_os_pwm_apply(const void *ctrl, uint32_t channel, uint32_t period_ns,
		     uint32_t duty_ns, bool inverted, bool enable)
{
	struct rt_ofw_node *np = (struct rt_ofw_node *)ctrl;
	struct rt_device_pwm *pwm;

	(void)inverted;
	if (!np)
		return -ENOTSUP;
	if (!rt_ofw_data(np))
		rt_platform_ofw_request(np);
	pwm = rt_ofw_data(np);
	if (!pwm)
		return -ENOTSUP;
	if (rt_pwm_set(pwm, channel, period_ns, duty_ns))
		return -EIO;
	return enable ? -rt_pwm_enable(pwm, channel) : -rt_pwm_disable(pwm, channel);
}

int dpy_os_regulator_set(uint32_t id, uint32_t microvolt, bool enable)
{
	(void)id;
	(void)microvolt;
	(void)enable;
	return -ENOTSUP;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
void dpy_os_vprintf(const char *fmt, va_list ap)
{
	char buf[200];

	vsnprintf(buf, sizeof(buf), fmt, ap);
	rt_kprintf("%s", buf);
}

void dpy_os_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	dpy_os_vprintf(fmt, ap);
	va_end(ap);
}
