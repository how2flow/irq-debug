/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) Steve Jeong <steve@how2flow.net>
 */

/*
 * GIC verification tests
 *
 *   typer / iidr        — distributor identity register readback
 *   ppi / ppi_repeated  — virtual timer (CNTV, INTID 27 = PPI 11) IRQ delivery
 *   spi / spi_repeated  — SPI 700 (INTID 732) trigger via GICD_ISPENDR
 *   lpi / lpi_repeated  — LPI 8192 trigger via ITS INT command
 *                          (real impl compiled only when CONFIG_GIC_V3_ITS=y;
 *                           returns SOC_TEST_SKIP in S-EL1 / BL2 builds)
 *
 * Zephyr system timer uses CNTP, so CNTV is free for test use.
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include "framework/soc_test.h"
#include "soc/soc_regs.h"

/* ----------------------------------------------------------------------------
 * Distributor register readback
 * ---------------------------------------------------------------------------- */

static enum soc_test_result test_gic_typer(void)
{
	uint32_t typer = SOC_TEST_REG_RD32(SOC_GICD_BASE + GICD_TYPER);
	uint32_t it_lines = typer & GICD_TYPER_ITLINES_MASK;

	SOC_TEST_CHECK(it_lines > 0, "GIC reports no interrupt lines");
	return SOC_TEST_PASS;
}

static enum soc_test_result test_gic_iidr(void)
{
	uint32_t iidr = SOC_TEST_REG_RD32(SOC_GICD_BASE + GICD_IIDR);

	SOC_TEST_CHECK(iidr != 0, "GIC IIDR is zero (invalid)");
	return SOC_TEST_PASS;
}

/* ----------------------------------------------------------------------------
 * PPI — Virtual Timer (CNTV)
 * ---------------------------------------------------------------------------- */

#define ARM_VIRT_TIMER_IRQ  27       /* INTID 27 = PPI 11 = EL1 vtimer */

#define CNTV_CTL_ENABLE     BIT(0)
#define CNTV_CTL_IMASK      BIT(1)

static struct k_sem ppi_sem;
static volatile uint32_t ppi_isr_count;

static void virt_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	/* Mask the timer interrupt to clear the pending state. */
	uint64_t ctl = CNTV_CTL_IMASK;
	__asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
	__asm__ volatile("isb");

	ppi_isr_count++;
	k_sem_give(&ppi_sem);
}

static enum soc_test_result test_gic_ppi(void)
{
	k_sem_init(&ppi_sem, 0, 1);
	ppi_isr_count = 0;

	irq_connect_dynamic(ARM_VIRT_TIMER_IRQ, 0, virt_timer_isr, NULL, 0);
	irq_enable(ARM_VIRT_TIMER_IRQ);

	/* Arm CNTV to fire quickly. */
	uint64_t tval = 1000;
	__asm__ volatile("msr cntv_tval_el0, %0" :: "r"(tval));
	uint64_t ctl = CNTV_CTL_ENABLE;
	__asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
	__asm__ volatile("isb");

	int ret = k_sem_take(&ppi_sem, K_MSEC(500));

	/* Cleanup. */
	ctl = 0;
	__asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
	irq_disable(ARM_VIRT_TIMER_IRQ);

	SOC_TEST_CHECK(ret == 0, "PPI (virt timer) IRQ not received");
	SOC_TEST_CHECK(ppi_isr_count > 0, "ISR was not called");
	return SOC_TEST_PASS;
}

static enum soc_test_result test_gic_ppi_repeated(void)
{
	const int repeat = 3;

	k_sem_init(&ppi_sem, 0, 1);
	ppi_isr_count = 0;

	irq_connect_dynamic(ARM_VIRT_TIMER_IRQ, 0, virt_timer_isr, NULL, 0);
	irq_enable(ARM_VIRT_TIMER_IRQ);

	for (int i = 0; i < repeat; i++) {
		uint64_t tval = 1000;
		__asm__ volatile("msr cntv_tval_el0, %0" :: "r"(tval));
		uint64_t ctl = CNTV_CTL_ENABLE;
		__asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
		__asm__ volatile("isb");

		int ret = k_sem_take(&ppi_sem, K_MSEC(500));
		if (ret != 0) {
			uint64_t off = 0;
			__asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(off));
			irq_disable(ARM_VIRT_TIMER_IRQ);
			SOC_TEST_CHECK(false, "PPI repeated: timeout on iteration");
		}
	}

	uint64_t ctl = 0;
	__asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(ctl));
	irq_disable(ARM_VIRT_TIMER_IRQ);

	SOC_TEST_CHECK_EQ(ppi_isr_count, (uint32_t)repeat, "ISR call count mismatch");
	return SOC_TEST_PASS;
}

/* ----------------------------------------------------------------------------
 * SPI — set-pending via GICD_ISPENDR
 *
 * Mirrors the Linux irq-test driver behaviour: pick a free SPI INTID, register
 * an ISR, then artificially set its pending bit and verify dispatch. Edge-
 * triggered configuration is used so each SET-PEND is consumed exactly once.
 * ---------------------------------------------------------------------------- */

#define SPI_TEST_INTID  700U   /* INTID = 32 + 700 = 732; pick any free SPI */

static struct k_sem spi_sem;
static volatile uint32_t spi_isr_count;

static void spi_test_isr(const void *arg)
{
	ARG_UNUSED(arg);
	spi_isr_count++;
	k_sem_give(&spi_sem);
}

static void spi_test_set_edge_trigger(uint32_t intid)
{
	uint32_t reg = SOC_GICD_BASE + GICD_ICFGR + 4U * (intid / 16U);
	uint32_t shift = (intid % 16U) * 2U;
	uint32_t val = SOC_TEST_REG_RD32(reg);

	val &= ~(0x3U << shift);
	val |= (GICD_ICFGR_EDGE << shift);
	SOC_TEST_REG_WR32(reg, val);
}

static void spi_test_set_pending(uint32_t intid)
{
	uint32_t reg = SOC_GICD_BASE + GICD_ISPENDR + 4U * (intid / 32U);

	SOC_TEST_REG_WR32(reg, BIT(intid % 32U));
}

static enum soc_test_result test_gic_spi(void)
{
	k_sem_init(&spi_sem, 0, 1);
	spi_isr_count = 0;

	spi_test_set_edge_trigger(SPI_TEST_INTID);
	irq_connect_dynamic(SPI_TEST_INTID, 0, spi_test_isr, NULL, 0);
	irq_enable(SPI_TEST_INTID);

	spi_test_set_pending(SPI_TEST_INTID);

	int ret = k_sem_take(&spi_sem, K_MSEC(500));

	irq_disable(SPI_TEST_INTID);

	SOC_TEST_CHECK(ret == 0, "SPI IRQ not received (timeout)");
	SOC_TEST_CHECK_EQ(spi_isr_count, 1U, "SPI ISR count mismatch");
	return SOC_TEST_PASS;
}

static enum soc_test_result test_gic_spi_repeated(void)
{
	const int repeat = 10;

	k_sem_init(&spi_sem, 0, 1);
	spi_isr_count = 0;

	spi_test_set_edge_trigger(SPI_TEST_INTID);
	irq_connect_dynamic(SPI_TEST_INTID, 0, spi_test_isr, NULL, 0);
	irq_enable(SPI_TEST_INTID);

	for (int i = 0; i < repeat; i++) {
		spi_test_set_pending(SPI_TEST_INTID);
		int ret = k_sem_take(&spi_sem, K_MSEC(500));
		if (ret != 0) {
			irq_disable(SPI_TEST_INTID);
			SOC_TEST_CHECK(false, "SPI repeated: timeout on iteration");
		}
	}

	irq_disable(SPI_TEST_INTID);

	SOC_TEST_CHECK_EQ(spi_isr_count, (uint32_t)repeat, "SPI ISR count mismatch");
	return SOC_TEST_PASS;
}

/* ----------------------------------------------------------------------------
 * LPI — set-pending via ITS INT command
 *
 * LPIs are Group 1 NS interrupts only, so the real implementation is gated by
 * CONFIG_GIC_V3_ITS (which forces CONFIG_ARMV8_A_NS). In Secure / BL2 builds
 * the test is registered but reports SOC_TEST_SKIP at run time.
 *
 * Required for the real path (NS build):
 *   - DT node with compatible "arm,gic-v3-its"
 *   - CONFIG_GIC_V3_ITS=y, CONFIG_HEAP_MEM_POOL_SIZE>=64K
 *   - CONFIG_NUM_IRQS large enough to address LPI INTID 8192
 * ---------------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_GIC_V3_ITS)

#include <zephyr/device.h>
#include <zephyr/drivers/interrupt_controller/gicv3_its.h>

#define LPI_TEST_INTID      8192U   /* fixed test LPI INTID */
#define LPI_TEST_DEVICE_ID  1U      /* arbitrary; unique on this ITS */
#define LPI_TEST_EVENT_ID   0U      /* one MSI/device, index 0 */

static struct k_sem lpi_sem;
static volatile uint32_t lpi_isr_count;
static bool lpi_inited;

static void lpi_test_isr(const void *arg)
{
	ARG_UNUSED(arg);
	lpi_isr_count++;
	k_sem_give(&lpi_sem);
}

/* Lazy ITS setup. Idempotent via lpi_inited guard. */
static int lpi_test_init(const struct device **its_out)
{
	const struct device *its = DEVICE_DT_GET_ANY(arm_gic_v3_its);
	int ret;

	if (its == NULL || !device_is_ready(its)) {
		return -ENODEV;
	}

	*its_out = its;
	if (lpi_inited) {
		return 0;
	}

	ret = its_setup_deviceid(its, LPI_TEST_DEVICE_ID, /*nites=*/1);
	if (ret) {
		return ret;
	}

	ret = its_map_intid(its, LPI_TEST_DEVICE_ID, LPI_TEST_EVENT_ID,
			    LPI_TEST_INTID);
	if (ret) {
		return ret;
	}

	irq_connect_dynamic(LPI_TEST_INTID, 0, lpi_test_isr, NULL, 0);
	irq_enable(LPI_TEST_INTID);

	lpi_inited = true;
	return 0;
}

static enum soc_test_result test_gic_lpi(void)
{
	const struct device *its;

	if (lpi_test_init(&its) != 0) {
		return SOC_TEST_FAIL;
	}

	k_sem_init(&lpi_sem, 0, 1);
	lpi_isr_count = 0;

	its_send_int(its, LPI_TEST_DEVICE_ID, LPI_TEST_EVENT_ID);

	int ret = k_sem_take(&lpi_sem, K_MSEC(500));

	SOC_TEST_CHECK(ret == 0, "LPI IRQ not received (timeout)");
	SOC_TEST_CHECK_EQ(lpi_isr_count, 1U, "LPI ISR count mismatch");
	return SOC_TEST_PASS;
}

static enum soc_test_result test_gic_lpi_repeated(void)
{
	const int repeat = 10;
	const struct device *its;

	if (lpi_test_init(&its) != 0) {
		return SOC_TEST_FAIL;
	}

	k_sem_init(&lpi_sem, 0, 1);
	lpi_isr_count = 0;

	for (int i = 0; i < repeat; i++) {
		its_send_int(its, LPI_TEST_DEVICE_ID, LPI_TEST_EVENT_ID);
		int ret = k_sem_take(&lpi_sem, K_MSEC(500));
		if (ret != 0) {
			SOC_TEST_CHECK(false, "LPI repeated: timeout on iteration");
		}
	}

	SOC_TEST_CHECK_EQ(lpi_isr_count, (uint32_t)repeat, "LPI ISR count mismatch");
	return SOC_TEST_PASS;
}

#else  /* !CONFIG_GIC_V3_ITS */

static enum soc_test_result test_gic_lpi(void)          { return SOC_TEST_SKIP; }
static enum soc_test_result test_gic_lpi_repeated(void) { return SOC_TEST_SKIP; }

#endif

SOC_TEST_DEFINE(gic_typer,        "gic", test_gic_typer,        SOC_TEST_FLAG_NONE);
SOC_TEST_DEFINE(gic_iidr,         "gic", test_gic_iidr,         SOC_TEST_FLAG_NONE);
SOC_TEST_DEFINE(gic_ppi,          "gic", test_gic_ppi,          SOC_TEST_FLAG_SKIP_EMULATOR);
SOC_TEST_DEFINE(gic_ppi_repeated, "gic", test_gic_ppi_repeated, SOC_TEST_FLAG_SKIP_EMULATOR);
SOC_TEST_DEFINE(gic_spi,          "gic", test_gic_spi,          SOC_TEST_FLAG_NONE);
SOC_TEST_DEFINE(gic_spi_repeated, "gic", test_gic_spi_repeated, SOC_TEST_FLAG_NONE);
SOC_TEST_DEFINE(gic_lpi,          "gic", test_gic_lpi,          SOC_TEST_FLAG_NONE);
SOC_TEST_DEFINE(gic_lpi_repeated, "gic", test_gic_lpi_repeated, SOC_TEST_FLAG_NONE);
