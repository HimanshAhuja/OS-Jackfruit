# ─────────────────────────────────────────────────────────────────
# Makefile — Multi-Container Runtime
#
# Targets:
#   all          — build engine binary + kernel module + workloads
#   engine       — user-space runtime only
#   workloads    — all test workloads
#   monitor.ko   — kernel module only
#   ci           — CI-safe compile check (no sudo / headers needed)
#   clean        — remove all build artefacts
# ─────────────────────────────────────────────────────────────────

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -pthread
KDIR    := /lib/modules/$(shell uname -r)/build

# Kernel module source
obj-m   += monitor.o

.PHONY: all engine workloads monitor.ko ci clean

all: engine workloads monitor.ko

# ── User-space binary ────────────────────────────────────────────
engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ $<

# ── Workloads ────────────────────────────────────────────────────
workloads: cpu_workload io_workload mem_workload

cpu_workload: cpu_workload.c
	$(CC) $(CFLAGS) -o $@ $<

io_workload: io_workload.c
	$(CC) $(CFLAGS) -o $@ $<

mem_workload: mem_workload.c
	$(CC) $(CFLAGS) -o $@ $<

# ── Kernel module ────────────────────────────────────────────────
monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ── CI target (GitHub Actions — no sudo, no kernel headers) ──────
ci: engine workloads
	@echo "CI build OK: engine + workloads compiled successfully"

# ── Clean ────────────────────────────────────────────────────────
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f engine cpu_workload io_workload mem_workload
