// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * ARM GICv3 SPI runtime trigger.
 *
 * Binds to "arm,gic-v3-spi-test", consumes the SPI declared in DT
 * (interrupts = <GIC_SPI N flags ...>), and exposes
 * /sys/kernel/debug/interrupts/spi/<name>/:
 *   - trigger:  write N → inject N pending events via
 *               irq_set_irqchip_state(IRQCHIP_STATE_PENDING, true) →
 *               gic_irq_set_irqchip_state → GICD_ISPENDR write.
 *   - info:     virq, hwirq (= 32 + SPI offset), counters, affinity.
 *   - affinity: read/write CPU list (mirrors /proc/irq/<virq>/smp_affinity_list).
 */

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "irq-debug.h"

#define DRIVER_NAME "gic-v3-spi-test"

#define GIC_SPI_INTID_BASE	32

struct irq_test_spi_dev {
	struct device		*dev;
	int			virq;
	unsigned long		hwirq;
	atomic64_t		trigger_cnt;
	atomic64_t		handler_cnt;
	struct dentry		*dir;
};

static atomic_t irq_test_spi_index = ATOMIC_INIT(0);

static irqreturn_t irq_test_spi_handler(int irq, void *data)
{
	struct irq_test_spi_dev *idev = data;

	atomic64_inc(&idev->handler_cnt);
	dev_info(idev->dev, "hello (SPI) virq=%d\n", irq);

	return IRQ_HANDLED;
}

static ssize_t irq_test_spi_trigger_write(struct file *file,
					  const char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct irq_test_spi_dev *idev = file->private_data;
	u64 n, i;
	int ret;

	ret = kstrtou64_from_user(ubuf, count, 0, &n);
	if (ret)
		return ret;
	if (n == 0)
		n = 1;

	for (i = 0; i < n; i++) {
		ret = irq_set_irqchip_state(idev->virq,
					    IRQCHIP_STATE_PENDING, true);
		if (ret) {
			dev_err(idev->dev,
				"irq_set_irqchip_state failed at %llu/%llu: %d\n",
				i, n, ret);
			return ret;
		}
		atomic64_inc(&idev->trigger_cnt);
	}

	return count;
}

static const struct file_operations irq_test_spi_trigger_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= irq_test_spi_trigger_write,
};

static ssize_t irq_test_spi_affinity_read(struct file *file, char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct irq_test_spi_dev *idev = file->private_data;
	struct irq_data *d = irq_get_irq_data(idev->virq);
	char buf[128];
	int len;

	if (!d)
		return -ENODEV;

	len = scnprintf(buf, sizeof(buf), "%*pbl\n",
			cpumask_pr_args(irq_data_get_affinity_mask(d)));

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t irq_test_spi_affinity_write(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	struct irq_test_spi_dev *idev = file->private_data;
	cpumask_var_t mask;
	int ret;

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	ret = cpumask_parselist_user(ubuf, count, mask);
	if (ret)
		goto out;

	ret = irq_set_affinity(idev->virq, mask);
out:
	free_cpumask_var(mask);
	return ret ? ret : count;
}

static const struct file_operations irq_test_spi_affinity_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= irq_test_spi_affinity_read,
	.write	= irq_test_spi_affinity_write,
};

static int irq_test_spi_info_show(struct seq_file *s, void *unused)
{
	struct irq_test_spi_dev *idev = s->private;
	struct irq_data *d = irq_get_irq_data(idev->virq);
	unsigned long spi_id = idev->hwirq >= GIC_SPI_INTID_BASE ?
			       idev->hwirq - GIC_SPI_INTID_BASE : 0;

	seq_puts(s,   "type:        SPI\n");
	seq_printf(s, "device:      %s\n", dev_name(idev->dev));
	seq_printf(s, "virq:        %d\n", idev->virq);
	seq_printf(s, "hwirq:       %lu\n", idev->hwirq);
	seq_printf(s, "spi_id:      %lu\n", spi_id);
	seq_printf(s, "trigger_cnt: %lld\n",
		   (long long)atomic64_read(&idev->trigger_cnt));
	seq_printf(s, "handler_cnt: %lld\n",
		   (long long)atomic64_read(&idev->handler_cnt));
	if (d) {
		seq_printf(s, "affinity:    %*pbl\n",
			   cpumask_pr_args(irq_data_get_affinity_mask(d)));
		seq_printf(s, "effective:   %*pbl\n",
			   cpumask_pr_args(irq_data_get_effective_affinity_mask(d)));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(irq_test_spi_info);

static void irq_test_spi_debugfs_register(struct irq_test_spi_dev *idev)
{
	struct dentry *parent = irq_debug_spi_dir();
	char name[16];

	if (IS_ERR_OR_NULL(parent))
		return;

	snprintf(name, sizeof(name), "spi-test%d",
		 atomic_fetch_inc(&irq_test_spi_index));

	idev->dir = debugfs_create_dir(name, parent);
	debugfs_create_file("trigger", 0200, idev->dir, idev,
			    &irq_test_spi_trigger_fops);
	debugfs_create_file("info", 0444, idev->dir, idev,
			    &irq_test_spi_info_fops);
	debugfs_create_file("affinity", 0644, idev->dir, idev,
			    &irq_test_spi_affinity_fops);
}

static int irq_test_spi_probe(struct platform_device *pdev)
{
	struct irq_test_spi_dev *idev;
	struct irq_data *d;
	int ret;

	idev = devm_kzalloc(&pdev->dev, sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return -ENOMEM;

	idev->dev = &pdev->dev;
	atomic64_set(&idev->trigger_cnt, 0);
	atomic64_set(&idev->handler_cnt, 0);
	platform_set_drvdata(pdev, idev);

	idev->virq = platform_get_irq(pdev, 0);
	if (idev->virq < 0)
		return dev_err_probe(&pdev->dev, idev->virq,
				     "missing IRQ in DT\n");

	d = irq_get_irq_data(idev->virq);
	idev->hwirq = d ? d->hwirq : 0;

	ret = devm_request_irq(&pdev->dev, idev->virq, irq_test_spi_handler,
			       0, dev_name(&pdev->dev), idev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request IRQ %d\n", idev->virq);

	irq_test_spi_debugfs_register(idev);

	dev_info(&pdev->dev,
		 "SPI test ready: virq=%d, hwirq=%lu (SPI %lu)\n",
		 idev->virq, idev->hwirq,
		 idev->hwirq >= GIC_SPI_INTID_BASE ?
			idev->hwirq - GIC_SPI_INTID_BASE : 0);

	return 0;
}

static void irq_test_spi_remove(struct platform_device *pdev)
{
	struct irq_test_spi_dev *idev = platform_get_drvdata(pdev);

	debugfs_remove_recursive(idev->dir);
}

static const struct of_device_id irq_test_spi_of_match[] = {
	{ .compatible = "arm,gic-v3-spi-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, irq_test_spi_of_match);

static struct platform_driver irq_test_spi_driver = {
	.probe	= irq_test_spi_probe,
	.remove	= irq_test_spi_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= irq_test_spi_of_match,
	},
};
module_platform_driver(irq_test_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong <steve@how2flow.net>");
MODULE_DESCRIPTION("ARM GICv3 SPI runtime trigger via GICD_ISPENDR");
