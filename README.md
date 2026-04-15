# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor process and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Himansh Ahuja | PES2UG24CS923 |
| Akarsh Parashuram | PES2UG24CS040 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. No WSL.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) flex bison libssl-dev libelf-dev gcc-12
```

### Build everything

```bash
make all
```

If the kernel module fails with gcc version errors, use:

```bash
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules CC=gcc-12
```

This produces:
- `engine` — user-space supervisor and CLI binary
- `monitor.ko` — kernel module
- `cpu_workload`, `io_workload`, `mem_workload` — workload binaries

Workload binaries must be statically linked to run inside Alpine containers:

```bash
gcc -O2 -static -o cpu_workload cpu_workload.c
gcc -O2 -static -o mem_workload mem_workload.c
gcc -O2 -static -o io_workload io_workload.c
```

### Prepare root filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

cp cpu_workload mem_workload io_workload ./rootfs-alpha/
cp cpu_workload mem_workload io_workload ./rootfs-beta/
```

### Load the kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
sudo dmesg | tail -3
```

Expected:
```
container_monitor: loaded (major=240)
```

### Start the supervisor

In a dedicated terminal (Terminal 1):

```bash
sudo ./engine supervisor ./rootfs-base
```

Expected:
```
[supervisor] kernel monitor ready
[supervisor] pid=... listening on /tmp/engine.sock
```

### Launch containers

In a second terminal (Terminal 2):

```bash
# Start two background containers
sudo ./engine start alpha ./rootfs-alpha /cpu_workload --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /cpu_workload --soft-mib 64 --hard-mib 96 --nice 10

# List running containers
sudo ./engine ps

# Inspect logs
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
```

### Run memory limit test

```bash
sudo ./engine start memtest ./rootfs-alpha /mem_workload 200 --soft-mib 20 --hard-mib 40
sudo dmesg -w   # watch soft and hard limit events appear
sudo ./engine ps
```

### Run scheduling experiment

```bash
sudo ./engine start hi ./rootfs-alpha /cpu_workload 500000 --nice 0
sudo ./engine start lo ./rootfs-beta  /cpu_workload 500000 --nice 10
# wait ~30 seconds then:
sudo ./engine logs hi
sudo ./engine logs lo
```

### Clean up

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Ctrl+C the supervisor in Terminal 1
sudo rmmod monitor
sudo dmesg | tail -5
```

---

## 3. Demo Screenshots

### Screenshot 1 — Kernel module loaded

![Kernel Module Loaded](Screenshots/SS1-Kernel-Module.png)

*`/dev/container_monitor` device appears after `sudo insmod monitor.ko`. `dmesg` confirms `container_monitor: loaded (major=240)`. This verifies the LKM loaded successfully and the character device is available for ioctl communication.*

---

### Screenshot 2 — Multi-container supervision and metadata tracking

![Metadata Tracking](Screenshots/SS2-Metadata-Tracking.png)

*Output of `sudo ./engine ps` showing containers alpha and beta with their host PIDs, states, soft/hard memory limits, start times, nice values, and stop reasons. Both containers were started with different configurations and tracked concurrently under one supervisor process (pid=8069).*

---

### Screenshot 3 — Bounded-buffer logging

![Bounded Buffer Logging](Screenshots/SS3-Bounded-Buffer-Logging.png)

*Contents of the container log captured through the producer-consumer logging pipeline. `sudo ./engine logs alpha3` and `cat /tmp/engine_logs/alpha3.log` both show `[cpu_workload] pid=1 found 41538 primes in 0.108 seconds`. The container stdout was captured via pipe → producer thread → bounded buffer → consumer thread → log file.*

---

### Screenshot 4 — CLI and IPC

![CLI and IPC](Screenshots/SS4-CLI-and-IPC.png)

*`sudo ./engine stop alpha4` issued from the CLI client travels over the UNIX domain socket (`/tmp/engine.sock`) to the supervisor. `engine ps` confirms alpha4 state is `killed` with reason `stopped`, showing the supervisor correctly received and acted on the stop command.*

---

### Screenshot 5 — Soft-limit warning

![Soft Limit](Screenshots/SS5-Soft-Limit.png)

*`dmesg` showing `container_monitor: registered pid=9643 soft=20 hard=40` followed by `[SOFT LIMIT] pid=9643 rss=20MiB >= soft=20MiB`. The kernel module fired the warning the first time the container's RSS reached the soft threshold.*

---

### Screenshot 6 — Hard-limit enforcement

![Hard Limit](Screenshots/SS5-SS6-Limits.png)

*`dmesg` showing `[HARD LIMIT] pid=9643 rss=40MiB >= hard=40MiB — SIGKILL`. The kernel module sent SIGKILL when RSS reached the hard limit. `engine ps` subsequently shows the container with reason `hard_limit_killed`, correctly distinguishing it from a manual stop.*

---

### Screenshot 7 — Scheduling experiment

![Scheduling Experiment](Screenshots/SS7-Scheduling.png)

*Two containers ran identical `cpu_workload 500000` (primes to 500,000). Container `hi` (nice=0) completed in **0.108 seconds**. Container `lo` (nice=10) completed in **0.127 seconds**. The lower-priority container took ~18% longer, consistent with CFS proportional scheduling.*

---

### Screenshot 8 — Clean teardown

![Clean Teardown](Screenshots/SS8-Clean-Teardown.png)

*`engine ps` shows all containers in terminal states (exited/killed/stopped). `ps aux` shows no defunct/zombie processes. `dmesg` confirms `container_monitor: unregistered` for all PIDs and `container_monitor: unloaded` after `sudo rmmod monitor`. No resource leaks.*

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Process isolation is implemented using `clone()` with flags `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`. The PID namespace makes the container's init process appear as PID 1, hiding all host processes from view. The UTS namespace gives each container its own hostname. The mount namespace ensures that filesystem operations — particularly the `mount("proc", "/proc", "proc", 0, NULL)` call inside the container — affect only that container's mount table without touching the host's `/proc`.

Filesystem isolation uses `chroot()` to replace the container's root directory with a dedicated Alpine rootfs copy. Since each container gets its own writable copy (`rootfs-alpha`, `rootfs-beta`), filesystem writes in one container do not affect another. `chroot` combined with a mount namespace is sufficient to prevent directory traversal escapes for this project.

Despite these isolations, all containers share the same host kernel, scheduler, network stack, and hardware. The host retains full visibility of container processes via their host PIDs, which is what allows the supervisor to send signals and the kernel module to read RSS via `get_mm_rss()`.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because a container's exit status, logs, and metadata must outlive the container itself. Without a persistent parent, calling `wait()` would discard that information. The supervisor also owns the UNIX socket listener, the bounded-buffer logging pipeline, and the kernel monitor file descriptor — none of which can exist in a short-lived CLI process.

Container processes are created with `clone()` rather than `fork()` so that namespace flags can be applied in a single call. The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all ready children without blocking the accept loop. `WNOHANG` is critical — blocking in the signal handler would stall all CLI requests. `SA_NOCLDSTOP` suppresses SIGCHLD for stop/continue events, keeping the handler focused on exits only.

Before sending `SIGTERM` from the `stop` command, we set a `stop_requested` flag. This allows the SIGCHLD handler to distinguish a supervisor-initiated stop from a hard-limit kill: if `stop_requested` is set and the signal is SIGKILL, the reason is `stopped`; if `stop_requested` is not set and the signal is SIGKILL, the reason is `hard_limit_killed`.

### 4.3 IPC, Threads, and Synchronisation

**Path A — Logging (pipes + bounded buffer):**
Each container's stdout and stderr are redirected into a `pipe()` at `clone()` time using `dup2()`. A per-container producer thread reads from the pipe's read end and pushes chunks into a shared bounded buffer. A single consumer thread pops entries and writes them to per-container log files. Without synchronisation, two producers could simultaneously read `count`, both increment to the same value, and one log entry would be silently lost. We protect the buffer with a `pthread_mutex_t` for mutual exclusion and two `pthread_cond_t` variables: `not_full` (prevents overwriting unread entries when the buffer is full) and `not_empty` (prevents the consumer from spinning when empty). Condition variables allow blocked threads to sleep rather than busy-wait, reducing CPU overhead.

**Path B — Control (UNIX domain socket):**
Each CLI process connects to `/tmp/engine.sock`, sends a command string, reads the response, and exits. The supervisor dispatches commands synchronously in its accept loop. This path is completely independent of the logging pipes, satisfying the requirement for two distinct IPC mechanisms.

**Metadata synchronisation:**
The container list is shared between the accept thread, SIGCHLD handler, and producer threads. It is protected by a separate `pthread_mutex_t` (`g_containers_lock`). Keeping this lock independent from the buffer lock prevents deadlocks. A mutex is chosen over a spinlock because threads may sleep while holding it (e.g., the grace-period loop in `cmd_stop`); spinlocks cannot be held across a sleep.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped and present in RAM for a process. It excludes pages swapped to disk, shared library pages not yet faulted in, and reserved-but-untouched virtual memory. RSS is therefore the most practical per-process metric for memory enforcement.

Soft and hard limits represent different policy goals. A soft limit is a warning threshold: the kernel module logs a `pr_warn` event the first time RSS crosses it, allowing the operator to observe the breach without interrupting the container. A hard limit is a safety guarantee: once crossed, the module sends `SIGKILL` immediately, protecting other containers and the host from OOM pressure.

Enforcement belongs in kernel space for two reasons. First, user-space polling is subject to scheduling delays — a user-space monitor may not run for hundreds of milliseconds, during which a runaway process could exhaust memory and trigger the host OOM killer before our policy acts. The kernel timer fires independently of user-space scheduling. Second, `get_mm_rss()` is only available to kernel code; user space must parse `/proc/<pid>/status`, which is slower and subject to lock contention.

### 4.5 Scheduling Behaviour

Linux uses the Completely Fair Scheduler (CFS), which tracks a `vruntime` (virtual runtime) per task. At each scheduling decision CFS picks the task with the lowest `vruntime`. A higher nice value increases a task's weight penalty: its `vruntime` advances faster per unit of real time, so CFS selects it less often relative to lower-nice peers.

In our experiment, two containers each ran `cpu_workload 500000` (primes to 500,000). Container `hi` (nice=0) completed in **0.108 seconds**; container `lo` (nice=10) completed in **0.127 seconds** — approximately 18% slower. With nice=0 vs nice=10 the CFS weight ratio is approximately 1024:110 (~9:1), so on a single CPU `hi` receives roughly 9× more CPU time. The workload is short enough that `lo` may have had to wait for multiple scheduling rounds, amplifying the effect.

This demonstrates CFS's proportional fairness: lower-priority tasks are not starved but receive CPU time scaled by their weight, resulting in longer wall-clock completion times.

---

## 5. Design Decisions and Tradeoffs

### Namespace isolation — `chroot` vs `pivot_root`

**Choice:** `chroot()` after entering a mount namespace.

**Tradeoff:** A root process inside the container could escape using `chroot()` again or `..` traversal before the mount namespace is fully set up. `pivot_root` closes this escape but requires a valid separate mountpoint for the old root, adding setup complexity.

**Justification:** For a course project demonstrating the isolation mechanism, `chroot` combined with `CLONE_NEWNS` is sufficient and keeps the implementation clean. A production runtime would use `pivot_root` plus capability dropping.

---

### Supervisor architecture — single-threaded accept loop

**Choice:** Non-blocking `accept()` with `O_NONBLOCK`, polling `g_supervisor_alive` every 50ms.

**Tradeoff:** A slow command (e.g., `run` waiting for container exit) blocks the accept loop for other CLI clients during its polling intervals.

**Justification:** CLI commands are infrequent. The `run` command polls with 200ms sleep intervals, so it does not hold the CPU. For a production system, `epoll` with a thread pool would be appropriate.

---

### IPC and logging — pipes + bounded buffer

**Choice:** Per-container producer threads feeding a 512-slot shared bounded buffer, drained by one consumer thread.

**Tradeoff:** Thread overhead scales with container count (one thread per container). Under very high container counts, thread creation and context switching could become a bottleneck.

**Justification:** For up to ~64 containers the overhead is negligible. The bounded buffer decouples container execution speed from disk write latency — a slow disk write does not block the container's stdout.

---

### Kernel monitor — mutex vs spinlock

**Choice:** `DEFINE_MUTEX` to protect the container list inside the kernel module.

**Tradeoff:** A spinlock would be faster for short critical sections but cannot be held while sleeping. Our RSS check calls `get_task_mm()` and `mmput()`, which can sleep. Using a spinlock here would trigger a kernel `BUG()` in `schedule()`.

**Justification:** Correctness requires a mutex. The slightly higher overhead is acceptable for a 1-second timer callback.

---

### Scheduling experiments — nice values vs CPU affinity

**Choice:** `nice()` to vary scheduling priority rather than `sched_setaffinity`.

**Tradeoff:** Nice values demonstrate CFS weight-based fairness but the effect size depends on system load. CPU affinity would demonstrate isolation but is a different concept.

**Justification:** Nice-value experiments directly illustrate the CFS weighted-fairness mechanism described in the project spec, producing clear and reproducible timing differences.

---

## 6. Scheduler Experiment Results

### Experiment — CPU-bound containers with different nice values

Both containers ran `/cpu_workload 500000` — computing all primes up to 500,000 using trial division.

| Container | Nice value | Priority | Wall-clock time |
|-----------|-----------|----------|----------------|
| hi | 0 | Normal | **0.108 seconds** |
| lo | 10 | Low | **0.127 seconds** |

Both containers were started within seconds of each other so they competed for CPU time throughout their runtime.

**Observations:**

- `hi` finished ~18% faster despite running the same workload.
- Neither task was starved — both completed successfully.
- The difference is consistent with CFS proportional sharing: at nice=0 vs nice=10, the weight ratio is approximately 1024:110 (~9:1), meaning `hi` received a much larger share of available CPU time.

**Conclusion:** The Linux CFS scheduler correctly honoured the nice values by allocating proportionally more CPU time to the higher-priority container. The runtime's `--nice` flag successfully influenced scheduling behaviour through the `nice()` syscall applied before `execvp` in the container child process. This demonstrates that the runtime can serve as an experimental platform for observing scheduler behaviour under different priority configurations.
