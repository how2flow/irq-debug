# irq-debug

Kernel patches for runtime SW injection and latency benchmarking of ARM
GICv3 interrupts. Replaces JTAG-based workflows (writing INT/INV directly
into the ITS commandq, or poking GICD_ISPENDR) with simple debugfs writes.

Two consumer drivers share a small framework:

- **irq-test** — fires N interrupts, verifies handler invocation count.
- **irq-bench** — measures dispatch latency (ktime) over N iterations,
  reports avg/min/max in nanoseconds.

Both currently cover **LPI** (via ITS) and **SPI**. PPI/SGI test drivers
can be added later under the same framework.

## How it works

### Common path

```
echo ... > .../trigger    (test)            echo 1 > .../run    (bench)
        ↓                                            ↓
irq_set_irqchip_state(virq, IRQCHIP_STATE_PENDING, true)
        ↓
chip->irq_set_irqchip_state(...)
        ├── LPI (ITS) → its_send_int → ITS commandq INT(DeviceID, EventID)
        └── SPI (GIC) → gic_irq_set_irqchip_state → GICD_ISPENDR write
        ↓
HW dispatch → CPU IRQ → handler
```

### irq-test handler
Increments a counter and prints `hello (LPI/SPI) virq=N`. Goal: confirm
the trigger path works end-to-end.

### irq-bench handler
Captures `CNTPCT_EL0` (physical counter) at handler entry, signals a
`completion` so the caller wakes. Per-iteration delta = handler entry -
kernel-side timestamp just before `irq_set_irqchip_state` returned.
Measures dispatch latency from the caller's perspective.

`CNTPCT_EL0` is read directly via `read_sysreg(...)` instead of
`ktime_get()`. On arm64 `ktime_get()` reads `CNTVCT_EL0` (virtual
counter), which freezes while a vCPU is descheduled — a guest's
benchmark would then under-report latency because the time the
hypervisor spent handling/emulating GIC traps would not be counted.
`CNTPCT_EL0` is the host wall-clock counter and is unaffected by vCPU
scheduling, so the same code yields meaningful numbers both on bare
metal and inside a guest, with hypervisor cost folded in.

## Layout

```
linux/
├── arch/arm64/boot/dts/vendor/target/
│   ├── irq-test.dtsi              # test devices (LPI + SPI)
│   └── irq-bench.dtsi             # bench devices (LPI + SPI)
└── drivers/misc/
    ├── Makefile.add               # one line to append to drivers/misc/Makefile
    └── irq-debug/                 # framework + nested consumers
        ├── Makefile               # arm64-gated; descends into test/ and bench/
        ├── irq-debug.h            # public API
        ├── irq-debug.c            # /sys/kernel/debug/interrupts/ root + lpi/spi
        ├── irq-test/              # functional test consumers
        │   ├── Makefile           # ccflags -I.. picks up irq-debug.h
        │   ├── irq-test-lpi.c
        │   └── irq-test-spi.c
        └── irq-bench/             # latency benchmark consumers
            ├── Makefile           # ccflags -I..
            ├── irq-bench.h        # CNTPCT_EL0 / CNTFRQ_EL0 inline helpers
            ├── irq-bench-lpi.c
            └── irq-bench-spi.c
```

Resulting debugfs tree at runtime:

```
/sys/kernel/debug/interrupts/
├── lpi/
│   ├── its-test0/    trigger / info / affinity
│   └── its-bench0/   times / run / result / info / affinity
└── spi/
    ├── spi-test0/    trigger / info / affinity
    └── spi-bench0/   times / run / result / info / affinity
```

## Apply

From the target kernel tree root:

```bash
IRQ_USER=irq-debug/linux
VENDOR=<vendor>          # SoC vendor directory (e.g. arm, ti, qcom...)
TARGET=<target>          # SoC target directory
BOARD_DTS=arch/arm64/boot/dts/$VENDOR/$TARGET/<your-board>.dts

# 1. Copy the framework (carries irq-test/ and irq-bench/ inside)
cp -r $IRQ_USER/drivers/misc/irq-debug ./drivers/misc/

# 2. Copy the DT fragments (substitute the real vendor/target path)
cp $IRQ_USER/arch/arm64/boot/dts/vendor/target/irq-test.dtsi  \
   ./arch/arm64/boot/dts/$VENDOR/$TARGET/
cp $IRQ_USER/arch/arm64/boot/dts/vendor/target/irq-bench.dtsi \
   ./arch/arm64/boot/dts/$VENDOR/$TARGET/

# 3. Append one line to drivers/misc/Makefile
cat $IRQ_USER/drivers/misc/Makefile.add >> ./drivers/misc/Makefile

# 4. Add include lines to the board DTS you want (manual). Each is independent:
#       #include "irq-test.dtsi"
#       #include "irq-bench.dtsi"
```

## Dependencies

Build gates (per-file):

| Component | Kconfig |
|---|---|
| Framework (`irq-debug.o`) | `CONFIG_ARM_GIC_V3` |
| Test SPI (`irq-test-spi.o`) | `CONFIG_ARM_GIC_V3` |
| Test LPI (`irq-test-lpi.o`) | `CONFIG_ARM_GIC_V3_ITS` |
| Bench SPI (`irq-bench-spi.o`) | `CONFIG_ARM_GIC_V3` |
| Bench LPI (`irq-bench-lpi.o`) | `CONFIG_ARM_GIC_V3_ITS` |

The framework directory `irq-debug/` is gated by `CONFIG_ARM_GIC_V3` in
`drivers/misc/Makefile` (single line) and recursively descends into the
`irq-test/` and `irq-bench/` consumers. SPI alone builds without ITS
support.

The whole tree is additionally gated to `CONFIG_ARM64=y` inside
`drivers/misc/irq-debug/Makefile` because the bench helpers read
`CNTPCT_EL0` / `CNTFRQ_EL0` via the arm64 `read_sysreg()` macro. There
is no AArch32 fallback and there are effectively no GICv3-on-AArch32
platforms in scope.

Also required:
- `CONFIG_DEBUG_FS=y`
- A DT with an `arm,gic-v3` node labelled `gic`
- For LPI: an `arm,gic-v3-its` node labelled `its0`

## DT nodes

### irq-test.dtsi (functional test)
```dts
#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <dt-bindings/interrupt-controller/irq.h>

/ {
    spi_test0: spi-test {
        compatible = "arm,gic-v3-spi-test";
        interrupt-parent = <&gic>;
        interrupts = <GIC_SPI 700 IRQ_TYPE_LEVEL_HIGH>;
    };

    its_test0: its-test {
        compatible = "arm,gic-v3-its-test";
        msi-parent = <&its0 0x1>;
    };
};
```

### irq-bench.dtsi (latency bench)
```dts
#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <dt-bindings/interrupt-controller/irq.h>

/ {
    spi_bench0: spi-bench {
        compatible = "arm,gic-v3-spi-bench";
        interrupt-parent = <&gic>;
        interrupts = <GIC_SPI 701 IRQ_TYPE_LEVEL_HIGH>;
    };

    its_bench0: its-bench {
        compatible = "arm,gic-v3-its-bench";
        msi-parent = <&its0 0x2>;
    };
};
```

Notes:
- No `reg` / `@addr` — these are dummy devices, no MMIO regions used.
- LPI: `msi-parent` second cell is the DeviceID. Use distinct values for
  test (`0x1`) vs bench (`0x2`) so both can coexist.
- SPI: pick free SPI INTIDs (test=700, bench=701 are arbitrary).
- The example uses the standard 3-cell GIC binding (type, INTID, flags).
  If your SoC's GIC uses 4 cells (e.g. for `ppi-partitions` support on
  big.LITTLE), append a fourth cell `0` for the partition index.

## Runtime

### irq-test (count verification)

```bash
dmesg | grep -E "its-test|spi-test"
# its-test:  LPI test ready: virq=N, DeviceID=0x1, EventID=0x0
# spi-test:  SPI test ready: virq=M, hwirq=732 (SPI 700)

# Inject a single LPI / SPI
echo 1 > /sys/kernel/debug/interrupts/lpi/its-test0/trigger
echo 1 > /sys/kernel/debug/interrupts/spi/spi-test0/trigger

# Stress (1000 each)
echo 1000 > /sys/kernel/debug/interrupts/lpi/its-test0/trigger
echo 1000 > /sys/kernel/debug/interrupts/spi/spi-test0/trigger

# Check counters
cat /sys/kernel/debug/interrupts/lpi/its-test0/info
# trigger_cnt: 1001
# handler_cnt: 1001  ← must match
```

### irq-bench (latency measurement)

```bash
dmesg | grep -E "its-bench|spi-bench"
# its-bench: LPI bench ready: virq=N, DeviceID=0x2, EventID=0x0, default times=5000
# spi-bench: SPI bench ready: virq=M, hwirq=733 (SPI 701), default times=5000

# Configure iteration count (default 5000)
echo 10000 > /sys/kernel/debug/interrupts/lpi/its-bench0/times

# Run benchmark (blocks until done)
echo 1 > /sys/kernel/debug/interrupts/lpi/its-bench0/run

# Read result
cat /sys/kernel/debug/interrupts/lpi/its-bench0/result
# count:    10000
# total_ns: 12345678
# avg_ns:   1234
# min_ns:   980
# max_ns:   8200

# Inspect clocksource (info now reports it explicitly)
cat /sys/kernel/debug/interrupts/lpi/its-bench0/info | grep -E '^(clocksource|cntfrq):'
# clocksource: cntpct_el0
# cntfrq:      24000000 Hz

# Same for SPI
echo 1 > /sys/kernel/debug/interrupts/spi/spi-bench0/run
cat /sys/kernel/debug/interrupts/spi/spi-bench0/result
```

Under a hypervisor (KVM, Xen, …), the same numbers fold in vCPU
preempt and GIC trap-emulation cost, because `CNTPCT_EL0` ticks
against host wall clock regardless of guest scheduling state. This
makes the bench usable for cross-hypervisor latency comparison from
inside the guest without any host-side instrumentation.

### Affinity (both test and bench, both types)

```bash
# Read current requested mask
cat /sys/kernel/debug/interrupts/lpi/its-bench0/affinity
# 0-7

# Restrict to a single CPU
echo 2 > /sys/kernel/debug/interrupts/lpi/its-bench0/affinity

# CPU list ranges accepted (e.g. 0-3,5)
echo 0-3,5 > /sys/kernel/debug/interrupts/spi/spi-bench0/affinity

# `info` reports both requested and effective masks
grep -E '^(affinity|effective):' \
     /sys/kernel/debug/interrupts/lpi/its-bench0/info
```

The standard `/proc/irq/<virq>/smp_affinity[_list]` interface still works
unchanged.

## Future extensions

Type-specific files planned under `irq-test/` and `irq-bench/`:

| Type | Status | Trigger mechanism |
|---|---|---|
| LPI | current | `irq_set_irqchip_state` PENDING → ITS INT command |
| SPI | current | `irq_set_irqchip_state` PENDING → GICD_ISPENDR write |
| PPI | planned | per-CPU `request_percpu_irq` + smp_call_function for cross-CPU trigger |
| SGI | not feasible via standard DT (reserved for IPI infrastructure) |

`irq-debug.c` (framework) is reused unchanged across all consumers.
