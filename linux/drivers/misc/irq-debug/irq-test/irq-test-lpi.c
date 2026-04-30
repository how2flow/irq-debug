// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 *
 * ARM GICv3 ITS LPI runtime trigger.
 *
 * Binds to "arm,gic-v3-its-test", allocates one LPI through the platform
 * MSI infrastructure, and exposes /sys/kernel/debug/interrupts/lpi/its-test<N>/:
 *   - trigger:     write N to inject N INT commands via the ITS command
 *                  queue (irq_set_irqchip_state PENDING=true → its_send_int).
 *   - wrap:        write any value to run the commandq wrap-around check —
 *                  submits 2 × ring_entries INTs with per-iteration
 *                  serialisation, verifies handler_cnt == submit_cnt and
 *                  that wrap occurred (ring_entries discoverable only when
 *                  the ITS reg region is mappable via msi-parent).
 *   - wrap_result: last wrap-test stats (ring size, counts, CWRITER snaps).
 *   - info:        virq, DeviceID, EventID, MSI msg, counters, affinity.
 *   - affinity:    read/write CPU list (mirrors /proc/irq/<virq>/smp_affinity_list).
 */

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "irq-debug.h"

#define DRIVER_NAME	"gic-v3-its-test"
#define ITS_CMD_SIZE	32U
#define WRAP_TIMEOUT_MS	1000U

struct irq_test_lpi_dev {
	struct device		*dev;
	int			virq;
	u32			device_id;
	u32			event_id;
	struct msi_msg		msg;
	atomic64_t		trigger_cnt;
	atomic64_t		handler_cnt;
	struct dentry		*dir;

	/* wrap-test extension; its_base may be NULL (test then unavailable) */
	void __iomem		*its_base;
	u64			ring_size;	/* bytes */

	struct mutex		wrap_lock;
	struct completion	wrap_done;
	bool			wrap_in_progress;

	bool			wrap_has_result;
	u64			wrap_submit;
	u64			wrap_handlers;
	u64			wrap_cwriter_before;
	u64			wrap_cwriter_after;
	bool			wrap_pass;
};

static atomic_t irq_test_lpi_index = ATOMIC_INIT(0);

static irqreturn_t irq_test_lpi_handler(int irq, void *data)
{
	struct irq_test_lpi_dev *idev = data;

	atomic64_inc(&idev->handler_cnt);

	/*
	 * Wrap test runs thousands of triggers; print would flood dmesg
	 * and the per-iteration completion is what the loop waits on.
	 */
	if (READ_ONCE(idev->wrap_in_progress))
		complete(&idev->wrap_done);
	else
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

static u64 irq_test_lpi_read_cwriter(struct irq_test_lpi_dev *idev)
{
	/* GITS_CWRITER offset[19:5] is the byte offset (32-byte aligned). */
	return readq_relaxed(idev->its_base + GITS_CWRITER) & GENMASK_ULL(19, 5);
}

static int irq_test_lpi_wrap_run(struct irq_test_lpi_dev *idev)
{
	u64 baseline_handler;
	u64 cwriter_before, cwriter_after;
	u64 submit_target, i, issued = 0;
	int ret = 0;

	if (!idev->its_base || !idev->ring_size)
		return -EOPNOTSUPP;

	if (!mutex_trylock(&idev->wrap_lock))
		return -EBUSY;

	/* 2x ring entries guarantees at least two wraps. */
	submit_target = (idev->ring_size / ITS_CMD_SIZE) * 2;

	cwriter_before = irq_test_lpi_read_cwriter(idev);
	baseline_handler = atomic64_read(&idev->handler_cnt);

	WRITE_ONCE(idev->wrap_in_progress, true);

	for (i = 0; i < submit_target; i++) {
		reinit_completion(&idev->wrap_done);
		ret = irq_set_irqchip_state(idev->virq,
					    IRQCHIP_STATE_PENDING, true);
		if (ret) {
			dev_err(idev->dev,
				"wrap: trigger failed at %llu/%llu: %d\n",
				i, submit_target, ret);
			break;
		}
		atomic64_inc(&idev->trigger_cnt);

		if (!wait_for_completion_timeout(&idev->wrap_done,
				msecs_to_jiffies(WRAP_TIMEOUT_MS))) {
			dev_err(idev->dev,
				"wrap: handler timeout at %llu/%llu\n",
				i, submit_target);
			ret = -ETIMEDOUT;
			break;
		}
		issued++;
	}

	WRITE_ONCE(idev->wrap_in_progress, false);

	cwriter_after = irq_test_lpi_read_cwriter(idev);

	idev->wrap_submit = issued;
	idev->wrap_handlers = atomic64_read(&idev->handler_cnt) - baseline_handler;
	idev->wrap_cwriter_before = cwriter_before;
	idev->wrap_cwriter_after = cwriter_after;
	idev->wrap_pass = (idev->wrap_handlers == idev->wrap_submit) &&
			  (issued * ITS_CMD_SIZE >= idev->ring_size);
	idev->wrap_has_result = true;

	dev_info(idev->dev,
		 "wrap: submit=%llu handlers=%llu cwriter 0x%llx -> 0x%llx %s\n",
		 idev->wrap_submit, idev->wrap_handlers,
		 idev->wrap_cwriter_before, idev->wrap_cwriter_after,
		 idev->wrap_pass ? "PASS" : "FAIL");

	mutex_unlock(&idev->wrap_lock);
	return ret;
}

static ssize_t irq_test_lpi_wrap_write(struct file *file,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct irq_test_lpi_dev *idev = file->private_data;
	u64 dummy;
	int ret;

	ret = kstrtou64_from_user(ubuf, count, 0, &dummy);
	if (ret)
		return ret;

	ret = irq_test_lpi_wrap_run(idev);
	return ret ? ret : count;
}

static const struct file_operations irq_test_lpi_wrap_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= irq_test_lpi_wrap_write,
};

static int irq_test_lpi_wrap_result_show(struct seq_file *s, void *unused)
{
	struct irq_test_lpi_dev *idev = s->private;

	if (!idev->its_base) {
		seq_puts(s, "wrap test unavailable (no ITS reg mapping)\n");
		return 0;
	}

	mutex_lock(&idev->wrap_lock);
	seq_printf(s, "ring_size:      %llu bytes\n", idev->ring_size);
	seq_printf(s, "ring_entries:   %llu\n", idev->ring_size / ITS_CMD_SIZE);

	if (!idev->wrap_has_result) {
		seq_puts(s, "no result yet (write to 'wrap' to run)\n");
		goto out;
	}

	seq_printf(s, "submit_cnt:     %llu\n", idev->wrap_submit);
	seq_printf(s, "handler_cnt:    %llu\n", idev->wrap_handlers);
	seq_printf(s, "cwriter_before: 0x%llx\n", idev->wrap_cwriter_before);
	seq_printf(s, "cwriter_after:  0x%llx\n", idev->wrap_cwriter_after);
	seq_printf(s, "wraps_min:      %llu\n",
		   (idev->wrap_submit * ITS_CMD_SIZE) / idev->ring_size);
	seq_printf(s, "verdict:        %s\n",
		   idev->wrap_pass ? "PASS" : "FAIL");
out:
	mutex_unlock(&idev->wrap_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(irq_test_lpi_wrap_result);

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
	if (idev->its_base)
		seq_printf(s, "ring_size:   %llu bytes (%llu entries)\n",
			   idev->ring_size, idev->ring_size / ITS_CMD_SIZE);
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

/*
 * Best-effort: walk msi-parent to the ITS DT node, map its registers,
 * read GITS_CBASER to learn the commandq ring size. On any failure the
 * wrap test is silently disabled — base/normal trigger path still works.
 */
static void irq_test_lpi_setup_wrap(struct platform_device *pdev,
				    struct irq_test_lpi_dev *idev)
{
	struct of_phandle_args args;
	void __iomem *base;
	u64 cbaser;
	int rc;

	rc = of_parse_phandle_with_args(pdev->dev.of_node, "msi-parent",
					"#msi-cells", 0, &args);
	if (rc)
		return;

	base = devm_of_iomap(&pdev->dev, args.np, 0, NULL);
	of_node_put(args.np);
	if (IS_ERR_OR_NULL(base)) {
		dev_warn(&pdev->dev,
			 "wrap: cannot map ITS regs; wrap test disabled\n");
		return;
	}

	cbaser = readq_relaxed(base + GITS_CBASER);
	if (!(cbaser & GITS_CBASER_VALID)) {
		dev_warn(&pdev->dev,
			 "wrap: GITS_CBASER not valid; wrap test disabled\n");
		return;
	}

	idev->its_base = base;
	idev->ring_size = ((cbaser & 0xff) + 1) * SZ_4K;

	dev_info(&pdev->dev,
		 "wrap: ring_size=%llu bytes (%llu entries)\n",
		 idev->ring_size, idev->ring_size / ITS_CMD_SIZE);
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
	if (idev->its_base) {
		debugfs_create_file("wrap", 0200, idev->dir, idev,
				    &irq_test_lpi_wrap_fops);
		debugfs_create_file("wrap_result", 0444, idev->dir, idev,
				    &irq_test_lpi_wrap_result_fops);
	}
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
	mutex_init(&idev->wrap_lock);
	init_completion(&idev->wrap_done);
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
	irq_test_lpi_setup_wrap(pdev, idev);
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
