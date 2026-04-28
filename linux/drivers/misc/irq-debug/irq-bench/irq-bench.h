/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * Shared inline helpers for irq-bench drivers.
 *
 * Reads CNTPCT_EL0 (physical counter) directly, instead of ktime_get(),
 * which on arm64 reads CNTVCT_EL0 (virtual counter). The virtual counter
 * stops while a vCPU is descheduled, so under a hypervisor it understates
 * dispatch latency. CNTPCT keeps ticking against host wall clock and
 * captures hypervisor preempt/emulation cost — which is exactly what
 * this benchmark wants to measure.
 *
 * Build-gated to arm64 in drivers/misc/irq-debug/Makefile.
 */

#ifndef __IRQ_BENCH_H
#define __IRQ_BENCH_H

#include <linux/math64.h>
#include <linux/time64.h>
#include <linux/types.h>

#include <asm/barrier.h>
#include <asm/sysreg.h>

static inline u64 irq_bench_cycles(void)
{
	isb();
	return read_sysreg(cntpct_el0);
}

static inline u32 irq_bench_cntfrq(void)
{
	return read_sysreg(cntfrq_el0);
}

static inline u64 irq_bench_cycles_to_ns(u64 cycles, u32 cntfrq)
{
	return mul_u64_u32_div(cycles, NSEC_PER_SEC, cntfrq);
}

#endif /* __IRQ_BENCH_H */
