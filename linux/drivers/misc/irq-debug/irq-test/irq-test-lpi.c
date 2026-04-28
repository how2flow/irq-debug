// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * ARM GICv3 ITS LPI runtime trigger.
 *
 * Binds to "arm,gic-v3-its-test", allocates one LPI through the platform
 * MSI infrastructure, and exposes /sys/kernel/debug/interrupts/lpi/its-test<N>/:
 *   - trigger:  write N (decimal/hex) to inject N INT commands via the
 *               ITS command queue (irq_set_irqchip_state PENDING=true →
 *               its_send_int → INT path).
 *   - info:     virq, DeviceID, EventID, MSI msg, counters, affinity.
 *   - affinity: read/write CPU list (mirrors /proc/irq/<virq>/smp_affinity_list).
 */

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "irq-debug.h"

#define DRIVER_NAME "gic-v3-its-test"

struct irq_test_lpi_dev {
	struct device		*dev;
	int			virq;
	u32			device_id;
	u32			event_id;
	struct msi_msg		msg;
	atomic64_t		trigger_cnt;
	atomic64_t		handler_cnt;
	struct dentry		*dir;
};

static atomic_t irq_test_lpi_index = ATOMIC_INIT(0);

static irqreturn_t irq_test_lpi_handler(int irq, void *data)
{
	struct irq_test_lpi_dev *idev = data;

	atomic64_inc(&idev->handler_cnt);
	dev_info(idev->dev, "hello (LPI) virq=%d\n", irq);

	return IRQ_HANDLED;
}

static void irq_test_lpi_write_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct device *dev = msi_desc_to_dev(desc);
	struct irq_test_lpi_dev *idev = dev_get_drvdata(dev);

	if (idev)
		idev->msg = *msg;
	/* Dummy device — no MSI capability registers to program. */
}

static ssize_t irq_test_lpi_trigger_write(struct file *file,
					  const char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct irq_test_lpi_dev *idev = file->private_data;
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

static const struct file_operations irq_test_lpi_trigger_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= irq_test_lpi_trigger_write,
};

static ssize_t irq_test_lpi_affinity_read(struct file *file, char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	struct irq_test_lpi_dev *idev = file->private_data;
	struct irq_data *d = irq_get_irq_data(idev->virq);
	char buf[128];
	int len;

	if (!d)
		return -ENODEV;

	len = scnprintf(buf, sizeof(buf), "%*pbl\n",
			cpumask_pr_args(irq_data_get_affinity_mask(d)));

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t irq_test_lpi_affinity_write(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	struct irq_test_lpi_dev *idev = file->private_data;
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

static const struct file_operations irq_test_lpi_affinity_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= irq_test_lpi_affinity_read,
	.write	= irq_test_lpi_affinity_write,
};

static int irq_test_lpi_info_show(struct seq_file *s, void *unused)
{
	struct irq_test_lpi_dev *idev = s->private;
	struct irq_data *d = irq_get_irq_data(idev->virq);
	u64 addr = ((u64)idev->msg.address_hi << 32) | idev->msg.address_lo;

	seq_puts(s,   "type:        LPI\n");
	seq_printf(s, "device:      %s\n", dev_name(idev->dev));
	seq_printf(s, "virq:        %d\n", idev->virq);
	seq_printf(s, "device_id:   0x%x\n", idev->device_id);
	seq_printf(s, "event_id:    0x%x\n", idev->event_id);
	seq_printf(s, "msi_addr:    0x%llx\n", addr);
	seq_printf(s, "msi_data:    0x%x\n", idev->msg.data);
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
DEFINE_SHOW_ATTRIBUTE(irq_test_lpi_info);

static void irq_test_lpi_parse_dt(struct platform_device *pdev,
				  struct irq_test_lpi_dev *idev)
{
	struct of_phandle_args args;

	idev->event_id = 0; /* single LPI allocation, index 0 */

	if (!of_parse_phandle_with_args(pdev->dev.of_node, "msi-parent",
					"#msi-cells", 0, &args)) {
		if (args.args_count >= 1)
			idev->device_id = args.args[0];
		of_node_put(args.np);
	}
}

static void irq_test_lpi_debugfs_register(struct irq_test_lpi_dev *idev)
{
	struct dentry *parent = irq_debug_lpi_dir();
	char name[16];

	if (IS_ERR_OR_NULL(parent))
		return;

	snprintf(name, sizeof(name), "its-test%d",
		 atomic_fetch_inc(&irq_test_lpi_index));

	idev->dir = debugfs_create_dir(name, parent);
	debugfs_create_file("trigger", 0200, idev->dir, idev,
			    &irq_test_lpi_trigger_fops);
	debugfs_create_file("info", 0444, idev->dir, idev,
			    &irq_test_lpi_info_fops);
	debugfs_create_file("affinity", 0644, idev->dir, idev,
			    &irq_test_lpi_affinity_fops);
}

static int irq_test_lpi_probe(struct platform_device *pdev)
{
	struct irq_test_lpi_dev *idev;
	int ret;

	idev = devm_kzalloc(&pdev->dev, sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return -ENOMEM;

	idev->dev = &pdev->dev;
	atomic64_set(&idev->trigger_cnt, 0);
	atomic64_set(&idev->handler_cnt, 0);
	platform_set_drvdata(pdev, idev);

	if (!pdev->dev.msi.domain)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "MSI domain unset; check msi-parent in DT\n");

	ret = platform_device_msi_init_and_alloc_irqs(&pdev->dev, 1,
						      irq_test_lpi_write_msg);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to alloc LPI\n");

	idev->virq = msi_get_virq(&pdev->dev, 0);

	ret = devm_request_irq(&pdev->dev, idev->virq, irq_test_lpi_handler,
			       0, dev_name(&pdev->dev), idev);
	if (ret) {
		platform_device_msi_free_irqs_all(&pdev->dev);
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request IRQ %d\n", idev->virq);
	}

	irq_test_lpi_parse_dt(pdev, idev);
	irq_test_lpi_debugfs_register(idev);

	dev_info(&pdev->dev,
		 "LPI test ready: virq=%d, DeviceID=0x%x, EventID=0x%x\n",
		 idev->virq, idev->device_id, idev->event_id);

	return 0;
}

static void irq_test_lpi_remove(struct platform_device *pdev)
{
	struct irq_test_lpi_dev *idev = platform_get_drvdata(pdev);

	debugfs_remove_recursive(idev->dir);
	platform_device_msi_free_irqs_all(&pdev->dev);
}

static const struct of_device_id irq_test_lpi_of_match[] = {
	{ .compatible = "arm,gic-v3-its-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, irq_test_lpi_of_match);

static struct platform_driver irq_test_lpi_driver = {
	.probe	= irq_test_lpi_probe,
	.remove	= irq_test_lpi_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= irq_test_lpi_of_match,
	},
};
module_platform_driver(irq_test_lpi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong <steve@how2flow.net>");
MODULE_DESCRIPTION("ARM GICv3 ITS LPI runtime trigger via INT command");
