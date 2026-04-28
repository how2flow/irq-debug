# SPDX-License-Identifier: GPL-2.0-or-later

# Cycle-counter access in irq-bench (read_sysreg(cntpct_el0)) is arm64-only,
# and there are effectively no GICv3-on-AArch32 platforms in this tree.
ifeq ($(CONFIG_ARM64),y)
obj-$(CONFIG_ARM_GIC_V3)	+= irq-debug.o
obj-$(CONFIG_ARM_GIC_V3)	+= irq-test/
obj-$(CONFIG_ARM_GIC_V3)	+= irq-bench/
endif
