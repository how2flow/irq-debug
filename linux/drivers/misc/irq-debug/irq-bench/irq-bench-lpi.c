// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * ARM GICv3 ITS LPI latency benchmark.
 *
 * Binds to "arm,gic-v3-its-bench", allocates one LPI through the platform
 * MSI infrastructure, and exposes /sys/kernel/debug/interrupts/lpi/its-bench<N>/:
 *   - times:    rw, iteration count for the next run (default 5000).
 *   - run:      w,  any value kicks off a run; blocks until completion.
 *   - result:   r,  last run statistics (count, total_ns, avg/min/max).
 *   - info:     r,  device metadata (virq, DeviceID, EventID, MSI msg).
 *   - affinity: rw, CPU list (mirrors /proc/irq/<virq>/smp_affinity_list).
 *
 * Per-iteration latency = CNTPCT at handler entry - CNTPCT just before
 * irq_set_irqchip_state() is called. Measures dispatch latency from the
 * caller's perspective. CNTPCT (physical counter) is used so that vCPU
 * preempt / hypervisor emulation cost is included under virtualization.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
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
#include "irq-bench.h"

#define DRIVER_NAME		"gic-v3-its-bench"
#define IRQ_BENCH_DEFAULT_TIMES	5000U

struct irq_bench_lpi_dev {
	struct device		*dev;
	int			virq;
	u32			device_id;
	u32			event_id;
	struct msi_msg		msg;
	struct dentry		*dir;

	/* per-iteration */
	struct completion	done;
	u64			handler_cycles;

	/* configuration */
	u32			cntfrq;
	u64			times;

	/* serialise concurrent runs */
	struct mutex		run_lock;

	/* last result */
	struct mutex		result_lock;
	bool			has_result;
	u64			result_count;
	u64			result_total_ns;
	u64			result_min_ns;
	u64			result_max_ns;
};

static atomic_t irq_bench_lpi_index = ATOMIC_INIT(0);

static irqreturn_t irq_bench_lpi_handler(int irq, void *data)
{
	struct irq_bench_lpi_dev *idev = data;

	idev->handler_cycles = irq_bench_cycles();
	complete(&idev->done);

	return IRQ_HANDLED;
}

static void irq_bench_lpi_write_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct device *dev = msi_desc_to_dev(desc);
	struct irq_bench_lpi_dev *idev = dev_get_drvdata(dev);

	if (idev)
		idev->msg = *msg;
}

static int irq_bench_lpi_run(struct irq_bench_lpi_dev *idev, u64 n)
{
	u64 i, total = 0, min = U64_MAX, max = 0;
	int ret = 0;

	if (n == 0)
		return -EINVAL;

	if (!mutex_trylock(&idev->run_lock))
		return -EBUSY;

	for (i = 0; i < n; i++) {
		u64 start;
		u64 delta;

		reinit_completion(&idev->done);
		start = irq_bench_cycles();
		ret = irq_set_irqchip_state(idev->virq,
					    IRQCHIP_STATE_PENDING, true);
		if (ret) {
			dev_err(idev->dev,
				"irq_set_irqchip_state failed at %llu/%llu: %d\n",
				i, n, ret);
			goto out;
		}

		wait_for_completion(&idev->done);
		delta = irq_bench_cycles_to_ns(idev->handler_cycles - start,
					       idev->cntfrq);
		total += delta;
		if (delta < min)
			min = delta;
		if (delta > max)
			max = delta;
	}

	mutex_lock(&idev->result_lock);
	idev->has_result = true;
	idev->result_count = n;
	idev->result_total_ns = total;
	idev->result_min_ns = min;
	idev->result_max_ns = max;
	mutex_unlock(&idev->result_lock);

out:
	mutex_unlock(&idev->run_lock);
	return ret;
}

static ssize_t times_read(struct file *file, char __user *ubuf,
			  size_t count, loff_t *ppos)
{
	struct irq_bench_lpi_dev *idev = file->private_data;
	char buf[24];
	int len;

	len = scnprintf(buf, sizeof(buf), "%llu\n", idev->times);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t times_write(struct file *file, const char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct irq_bench_lpi_dev *idev = file->private_data;
	u64 n;
	int ret;

	ret = kstrtou64_from_user(ubuf, count, 0, &n);
	if (ret)
		return ret;
	if (n == 0)
		return -EINVAL;

	idev->times = n;
	return count;
}

static const struct file_operations times_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= times_read,
	.write	= times_write,
};

static ssize_t run_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	struct irq_bench_lpi_dev *idev = file->private_data;
	u64 dummy;
	int ret;

	ret = kstrtou64_from_user(ubuf, count, 0, &dummy);
	if (ret)
		return ret;

	ret = irq_bench_lpi_run(idev, idev->times);
	return ret ? ret : count;
}

static const struct file_operations run_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= run_write,
};

static int result_show(struct seq_file *s, void *unused)
{
	struct irq_bench_lpi_dev *idev = s->private;

	mutex_lock(&idev->result_lock);
	if (!idev->has_result) {
		seq_puts(s, "no result yet (write to 'run' to benchmark)\n");
	} else {
		u64 avg = div_u64(idev->result_total_ns, idev->result_count);

		seq_printf(s, "count:    %llu\n", idev->result_count);
		seq_printf(s, "total_ns: %llu\n", idev->result_total_ns);
		seq_printf(s, "avg_ns:   %llu\n", avg);
		seq_printf(s, "min_ns:   %llu\n", idev->result_min_ns);
		seq_printf(s, "max_ns:   %llu\n", idev->result_max_ns);
	}
	mutex_unlock(&idev->result_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(result);

static int info_show(struct seq_file *s, void *unused)
{
	struct irq_bench_lpi_dev *idev = s->private;
	struct irq_data *d = irq_get_irq_data(idev->virq);
	u64 addr = ((u64)idev->msg.address_hi << 32) | idev->msg.address_lo;

	seq_puts(s,   "type:        LPI (bench)\n");
	seq_printf(s, "device:      %s\n", dev_name(idev->dev));
	seq_printf(s, "virq:        %d\n", idev->virq);
	seq_printf(s, "device_id:   0x%x\n", idev->device_id);
	seq_printf(s, "event_id:    0x%x\n", idev->event_id);
	seq_printf(s, "msi_addr:    0x%llx\n", addr);
	seq_printf(s, "msi_data:    0x%x\n", idev->msg.data);
	seq_printf(s, "times:       %llu\n", idev->times);
	seq_puts(s,   "clocksource: cntpct_el0\n");
	seq_printf(s, "cntfrq:      %u Hz\n", idev->cntfrq);
	if (d) {
		seq_printf(s, "affinity:    %*pbl\n",
			   cpumask_pr_args(irq_data_get_affinity_mask(d)));
		seq_printf(s, "effective:   %*pbl\n",
			   cpumask_pr_args(irq_data_get_effective_affinity_mask(d)));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(info);

static ssize_t affinity_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct irq_bench_lpi_dev *idev = file->private_data;
	struct irq_data *d = irq_get_irq_data(idev->virq);
	char buf[128];
	int len;

	if (!d)
		return -ENODEV;

	len = scnprintf(buf, sizeof(buf), "%*pbl\n",
			cpumask_pr_args(irq_data_get_affinity_mask(d)));

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t affinity_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct irq_bench_lpi_dev *idev = file->private_data;
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

static const struct file_operations affinity_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= affinity_read,
	.write	= affinity_write,
};

static void irq_bench_lpi_parse_dt(struct platform_device *pdev,
				   struct irq_bench_lpi_dev *idev)
{
	struct of_phandle_args args;

	idev->event_id = 0;

	if (!of_parse_phandle_with_args(pdev->dev.of_node, "msi-parent",
					"#msi-cells", 0, &args)) {
		if (args.args_count >= 1)
			idev->device_id = args.args[0];
		of_node_put(args.np);
	}
}

static void irq_bench_lpi_debugfs_register(struct irq_bench_lpi_dev *idev)
{
	struct dentry *parent = irq_debug_lpi_dir();
	char name[16];

	if (IS_ERR_OR_NULL(parent))
		return;

	snprintf(name, sizeof(name), "its-bench%d",
		 atomic_fetch_inc(&irq_bench_lpi_index));

	idev->dir = debugfs_create_dir(name, parent);
	debugfs_create_file("times",    0644, idev->dir, idev, &times_fops);
	debugfs_create_file("run",      0200, idev->dir, idev, &run_fops);
	debugfs_create_file("result",   0444, idev->dir, idev, &result_fops);
	debugfs_create_file("info",     0444, idev->dir, idev, &info_fops);
	debugfs_create_file("affinity", 0644, idev->dir, idev, &affinity_fops);
}

static int irq_bench_lpi_probe(struct platform_device *pdev)
{
	struct irq_bench_lpi_dev *idev;
	int ret;

	idev = devm_kzalloc(&pdev->dev, sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return -ENOMEM;

	idev->dev = &pdev->dev;
	idev->times = IRQ_BENCH_DEFAULT_TIMES;
	idev->cntfrq = irq_bench_cntfrq();
	init_completion(&idev->done);
	mutex_init(&idev->run_lock);
	mutex_init(&idev->result_lock);
	platform_set_drvdata(pdev, idev);

	if (!pdev->dev.msi.domain)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "MSI domain unset; check msi-parent in DT\n");

	ret = platform_device_msi_init_and_alloc_irqs(&pdev->dev, 1,
						      irq_bench_lpi_write_msg);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to alloc LPI\n");

	idev->virq = msi_get_virq(&pdev->dev, 0);

	ret = devm_request_irq(&pdev->dev, idev->virq, irq_bench_lpi_handler,
			       0, dev_name(&pdev->dev), idev);
	if (ret) {
		platform_device_msi_free_irqs_all(&pdev->dev);
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request IRQ %d\n", idev->virq);
	}

	irq_bench_lpi_parse_dt(pdev, idev);
	irq_bench_lpi_debugfs_register(idev);

	dev_info(&pdev->dev,
		 "LPI bench ready: virq=%d, DeviceID=0x%x, EventID=0x%x, default times=%llu\n",
		 idev->virq, idev->device_id, idev->event_id, idev->times);

	return 0;
}

static void irq_bench_lpi_remove(struct platform_device *pdev)
{
	struct irq_bench_lpi_dev *idev = platform_get_drvdata(pdev);

	debugfs_remove_recursive(idev->dir);
	platform_device_msi_free_irqs_all(&pdev->dev);
}

static const struct of_device_id irq_bench_lpi_of_match[] = {
	{ .compatible = "arm,gic-v3-its-bench" },
	{ }
};
MODULE_DEVICE_TABLE(of, irq_bench_lpi_of_match);

static struct platform_driver irq_bench_lpi_driver = {
	.probe	= irq_bench_lpi_probe,
	.remove	= irq_bench_lpi_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= irq_bench_lpi_of_match,
	},
};
module_platform_driver(irq_bench_lpi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steve Jeong <steve@how2flow.net>");
MODULE_DESCRIPTION("ARM GICv3 ITS LPI latency benchmark");
