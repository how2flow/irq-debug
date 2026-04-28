// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * IRQ debug framework — debugfs root and per-type subdirs for runtime
 * IRQ test/benchmark drivers (irq-test, irq-bench).
 *
 * All directories are created lazily on first request and cached.
 * Concurrent first-touch from multiple consumer modules is serialised
 * by a single mutex.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/mutex.h>

#include "irq-debug.h"

static DEFINE_MUTEX(irq_debug_lock);
static struct dentry *irq_debug_root_dir;
static struct dentry *irq_debug_lpi;
static struct dentry *irq_debug_spi;

static struct dentry *get_root_locked(void)
{
	if (!irq_debug_root_dir)
		irq_debug_root_dir = debugfs_create_dir("interrupts", NULL);
	return irq_debug_root_dir;
}

static struct dentry *get_type_dir_locked(struct dentry **slot, const char *name)
{
	struct dentry *root;

	if (*slot)
		return *slot;

	root = get_root_locked();
	if (IS_ERR_OR_NULL(root))
		return NULL;

	*slot = debugfs_create_dir(name, root);
	return *slot;
}

struct dentry *irq_debug_root(void)
{
	struct dentry *r;

	mutex_lock(&irq_debug_lock);
	r = get_root_locked();
	mutex_unlock(&irq_debug_lock);

	return r;
}
EXPORT_SYMBOL_GPL(irq_debug_root);

struct dentry *irq_debug_lpi_dir(void)
{
	struct dentry *r;

	mutex_lock(&irq_debug_lock);
	r = get_type_dir_locked(&irq_debug_lpi, "lpi");
	mutex_unlock(&irq_debug_lock);

	return r;
}
EXPORT_SYMBOL_GPL(irq_debug_lpi_dir);

struct dentry *irq_debug_spi_dir(void)
{
	struct dentry *r;

	mutex_lock(&irq_debug_lock);
	r = get_type_dir_locked(&irq_debug_spi, "spi");
	mutex_unlock(&irq_debug_lock);

	return r;
}
EXPORT_SYMBOL_GPL(irq_debug_spi_dir);
