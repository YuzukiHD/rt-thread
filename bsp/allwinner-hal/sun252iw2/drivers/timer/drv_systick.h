/* SPDX-License-Identifier: Apache-2.0 */
#ifndef __DRV_SYSTICK_H__
#define __DRV_SYSTICK_H__
void drv_systick_init(unsigned long interval);
void rt_tick_interrupt_clear(void);
void drv_systick_isr(void);
#endif
