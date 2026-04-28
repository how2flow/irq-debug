/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * Shared debugfs framework for runtime IRQ test/benchmark drivers.
 * Provides /sys/kernel/debug/interrupts/ root and per-type subdirs
 * (lpi/, spi/, ...) consumed by irq-test and irq-bench drivers.
 */

#ifndef __IRQ_DEBUG_H
#define __IRQ_DEBUG_H

struct dentry;

struct dentry *irq_debug_root(void);
struct dentry *irq_debug_lpi_dir(void);
struct dentry *irq_debug_spi_dir(void);

#endif /* __IRQ_DEBUG_H */
