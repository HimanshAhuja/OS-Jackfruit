# Multi-Container Runtime

**Course:** Operating Systems Mini Project
**Team size:** 2 students

---

## 1. Team Information

| Name | SRN |
|------|-----|
| \<Himansh Ahuja\> | \<PES2UG24CS923\> |
| \<Akarsh Parashuram\> | \<PES2UG24CS040\> |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Secure Boot must be **OFF** (check: `mokutil --sb-state`). No WSL.

### Build everything

```bash
make all
# or just the user-space parts (no kernel headers needed):
make ci
```

### Prepare Alpine rootfs

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

### Load the kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor   # should appear
dmesg | tail -3                # confirm "container_monitor: loaded"
```

### Full demo run sequence

```bash
# 1. Create per-container writable rootfs copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# 2. Copy workloads into rootfs so they are accessible inside containers
cp cpu_workload mem_workload io_workload ./rootfs-alpha/
cp cpu_workload mem_workload io_workload ./rootfs-beta/

# 3. Start supervisor (leave running in a dedicated terminal)
sudo ./engine supervisor ./rootfs-base

# 4. In a second terminal — start containers
sudo ./engine start alpha ./rootfs-alpha /cpu_workload --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /cpu_workload --soft-mib 64 --hard-mib 96 --nice 10

# 5. Check status
sudo ./engine ps

# 6. View live logs
sudo ./engine logs alpha

# 7. Memory limit demo (will be killed by kernel module)
sudo ./engine start mem-test ./rootfs-alpha /mem_workload 120 --soft-mib 40 --hard-mib 64
# Watch dmesg in another terminal:
dmesg -w

# 8. Stop a container
sudo ./engine stop beta

# 9. Run a one-shot container and wait for it
sudo ./engine run quick ./rootfs-alpha /cpu_workload 100000

# 10. Scheduling experiment (two CPU-bound containers, different nice)
sudo ./engine start hi  ./rootfs-alpha /cpu_workload 500000 --nice 0
sudo ./engine start lo  ./rootfs-beta  /cpu_workload 500000 --nice 10
sudo ./engine ps   # observe both running; logs will show timing difference

# 11. Clean up
sudo ./engine stop hi
sudo ./engine stop lo
# Ctrl-C the supervisor or:
kill $(pgrep -f "engine supervisor")

# 12. Unload module
sudo rmmod monitor
dmesg | tail -3   # confirm "container_monitor: unloaded"
```

### Verify no zombies after teardown

```bash
ps aux | grep -E 'Z|defunct'
# should show nothing related to engine
```

---

## 3. Demo Screenshots

> Replace each row with an annotated screenshot from your VM.

| # | What it demonstrates | Caption |
|---|----------------------|---------|
| 1 | Multi-container supervision | Two containers (alpha, beta) running under one supervisor; `ps aux` shows a single `engine supervisor` parent and two child PIDs |
| 2 | Metadata tracking | Output of `sudo ./engine ps` showing both containers with state `running`, soft/hard limits, start time, and reason |
| 3 | Bounded-buffer logging | Contents of `/tmp/engine_logs/alpha.log`; strace or added debug prints showing producer pushing to buffer and consumer writing to file |
| 4 | CLI and IPC | `sudo ./engine stop beta` issued in terminal 2; supervisor in terminal 1 prints "stop signal sent"; `ps` shows beta as `killed` |
| 5 | Soft-limit warning | `dmesg` output showing `[SOFT LIMIT] pid=... rss=...MiB >= soft=...MiB` for the mem-test container |
| 6 | Hard-limit enforcement | `dmesg` showing `[HARD LIMIT] pid=... — sending SIGKILL`; subsequent `engine ps` shows container reason `hard_limit_killed` |
| 7 | Scheduling experiment | Side-by-side completion times for `hi` (nice=0) and `lo` (nice=10) containers computing the same workload; hi finishes measurably faster |
| 8 | Clean teardown | `ps aux` after supervisor exit showing no zombie (Z) processes; supervisor prints "exited cleanly" |

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Our runtime achieves process isolation by passing `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` to `clone()`. Each container child sees itself as PID 1 in its own PID namespace; the host kernel still assigns it a unique host PID, which is what we register with the kernel monitor and track in metadata. UTS namespace isolation gives each container its own hostname. Mount namespace isolation (`CLONE_NEWNS`) means that the `mount("proc", "/proc", "proc", 0, NULL)` call inside the container affects only that container's mount table; the host's `/proc` is untouched.

`chroot()` replaces the container's root directory with a per-container copy of the Alpine rootfs. This prevents the container from accessing the host filesystem by traversing upward. `pivot_root` would be more thorough (it prevents `..` escapes even before the mount namespace is fully set up), but `chroot` after entering a mount namespace is sufficient for this project. What the host kernel still shares with all containers: the kernel itself, the hardware, and the network stack unless `CLONE_NEWNET` is added. Containers on the same host therefore share the same network interfaces and IP address.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary for two reasons. First, a container's exit status and metadata must outlive the container itself — without a permanent parent, the information is lost when `wait()` is called. Second, the supervisor owns the bounded-buffer logging pipeline, the UNIX socket listener, and the kernel monitor file descriptor; these resources cannot exist in a short-lived CLI process.

Process creation uses `clone()` rather than `fork()` so we can specify namespace flags in a single call. After `clone()`, the parent retains the child's host PID in metadata and installs a `SIGCHLD` handler. The handler calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all ready children without blocking — the `WNOHANG` flag is critical to avoid stalling the accept loop. Reaping prevents zombie accumulation: a zombie is a process whose entry remains in the process table after exit because no parent has called `wait`. The `SA_NOCLDSTOP` flag suppresses `SIGCHLD` for stop/continue events (only generated by ptrace or job control), keeping the handler focused.

Signal delivery across the container boundary works because the supervisor knows each container's host PID. `kill(host_pid, SIGTERM)` delivers to the container's init process. Since the container runs in its own PID namespace, a `kill(1, SIGTERM)` from within the container would reach the same process.

### 4.3 IPC, Threads, and Synchronisation

**Path A — pipes + bounded buffer.**
Each container's stdout and stderr are redirected into a `pipe()` at `clone()` time. The write end is kept by the child; the read end is passed to a per-container producer thread. Without synchronisation, two producer threads could both read `count` as, say, 5, both increment to 6, and one entry would be lost. We protect the bounded buffer with a `pthread_mutex_t` for mutual exclusion and two `pthread_cond_t` variables (`not_full`, `not_empty`). `not_full` prevents a producer from overwriting unread entries when the buffer is at capacity (deadlock avoidance: the producer blocks rather than busy-waits). `not_empty` prevents the consumer from spinning when the buffer is empty. A mutex alone would suffice for correctness but would busy-wait; condition variables allow the OS to sleep blocked threads efficiently.

**Path B — UNIX domain socket.**
Each CLI process connects, writes a command string, and reads the response. The supervisor accepts one connection per iteration and dispatches synchronously. Multiple concurrent CLI commands are serialised by the single-threaded accept loop, which is acceptable given the expected usage pattern. The container metadata list (`g_containers`) is shared between the accept thread, the SIGCHLD handler, and producer threads; it is protected by `g_containers_lock`. The choice of mutex over spinlock is appropriate because threads may sleep while holding the lock (e.g., in `cmd_stop`'s grace-period loop); a spinlock cannot be held across a sleep.

**Possible race without synchronisation:** A producer thread reads a container's log path from metadata at the same instant the SIGCHLD handler updates its state. Without `g_containers_lock`, the producer could read a partially written struct. The mutex prevents this torn read.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical memory pages currently mapped and present in RAM for a process. It does not measure: pages swapped to disk, shared library pages counted in every process that maps them, or memory-mapped files that have not been faulted in. RSS is therefore an approximation of a process's true physical memory footprint, but it is the most practical metric for per-process enforcement.

Soft and hard limits represent different policy goals. A soft limit is a warning threshold: the system logs the event but does not interrupt execution, allowing the container to continue serving requests while an operator decides whether to act. A hard limit is a safety guarantee: once exceeded, the process is killed unconditionally to protect other containers and the host from memory pressure or OOM events.

Enforcement belongs in kernel space because user-space monitoring is subject to scheduling: a user-space monitor may not run for hundreds of milliseconds after a process crosses its limit, allowing runaway allocation to trigger the host OOM killer before our soft policy has a chance to act. The kernel timer callback runs in kernel context on a well-defined schedule (`check_interval_ms`), independent of user-space scheduling. Additionally, `get_mm_rss()` is only available to kernel code; user space would have to parse `/proc/<pid>/status`, which is slower and can be delayed by kernel lock contention.

### 4.5 Scheduling Behaviour

Linux uses the Completely Fair Scheduler (CFS). CFS tracks a `vruntime` (virtual runtime) per task; at each scheduling decision it picks the task with the lowest `vruntime`. A higher `nice` value increases the task's weight penalty: its `vruntime` advances faster per unit of real time, so CFS selects it less often relative to lower-nice peers.

**Experiment:** Two containers each running `cpu_workload 500000`. Container `hi` runs at nice=0, container `lo` at nice=10. With nice=0 vs nice=10, the weight ratio is approximately 1024:110 (CFS uses a table mapping nice values to weights). We therefore expect `hi` to receive roughly 9× more CPU time than `lo` when both are runnable.

**Expected result:** `hi` completes in roughly one-ninth the wall-clock time of `lo` when both start simultaneously, because `lo` is almost always preempted in favour of `hi` whenever both are runnable. If the workload is short enough to finish before `lo` gets its first full time slice, `lo` may appear to be fully starved for the duration. This demonstrates that CFS's fairness is proportional (weighted) rather than absolute: lower-priority tasks do get CPU time, but their effective throughput is scaled by the weight ratio.

---

## 5. Design Decisions and Tradeoffs

### Namespace isolation — `chroot` vs `pivot_root`

We chose `chroot` for filesystem isolation because it is simpler to implement correctly and sufficient when combined with `CLONE_NEWNS`. The concrete tradeoff is that a privileged process inside the container could use `chroot` again (or `..` tricks) to escape if running as root without additional seccomp/capability restrictions. `pivot_root` closes this escape but requires a valid new root with a separate mountpoint for the old root, adding setup complexity. Justification: for a course project demonstrating the mechanism, `chroot` is the right tradeoff; a production runtime would use `pivot_root` plus capability dropping.

### Supervisor architecture — single-threaded accept loop

The supervisor uses a non-blocking `accept()` loop on one thread rather than spawning a thread per connection. This avoids the complexity of per-connection thread lifecycle management and keeps the dispatch path simple. The tradeoff is that a slow command (e.g., `run` waiting for container exit) blocks the accept loop for all other CLI clients. Justification: CLI commands are infrequent and short; `run` polls with 200ms sleep intervals, so it does not hold the CPU. For a production system, a thread pool or `epoll`-based event loop would be appropriate.

### IPC for logging — pipes + bounded buffer vs `epoll` + direct write

Using a per-container producer thread and a shared bounded buffer decouples I/O latency from container execution: a slow disk write does not block the container's stdout. The tradeoff is thread overhead (one thread per container) and bounded-buffer contention when many containers produce output simultaneously. An `epoll` approach with one monitoring thread would use fewer threads but couples log-write latency back into the event loop. Justification: for up to ~64 containers the thread overhead is negligible and the decoupling benefit is significant.

### Kernel monitor — mutex vs spinlock

We use `DEFINE_MUTEX` to protect the container list inside the kernel module. A spinlock would be faster for short critical sections but cannot be held while sleeping — and our RSS check calls `get_task_mm()` / `mmput()`, which can sleep. Using a spinlock here would cause a kernel BUG() in `schedule()`. Justification: correctness requires mutex; the slightly higher overhead is acceptable for a 1-second timer callback.

### Scheduling experiment — nice values vs CPU affinity

We use `nice()` to vary scheduling priority rather than pinning containers to specific CPU cores with `sched_setaffinity`. Nice values allow both containers to run on any CPU, demonstrating CFS weight-based fairness, which is the core CFS mechanism. CPU affinity would demonstrate a different concept (isolation) and would require a multi-core machine with careful binding. Justification: nice-value experiments more directly illustrate the CFS scheduling goal described in the project spec.

---

## 6. Scheduler Experiment Results

### Experiment A — Two CPU-bound containers at different nice values

| Container | nice | Workload | Wall time (seconds) |
|-----------|------|----------|---------------------|
| hi        | 0    | primes to 500 000 | _measured_ |
| lo        | 10   | primes to 500 000 | _measured_ |

**How to reproduce:**
```bash
sudo ./engine start hi ./rootfs-alpha /cpu_workload 500000 --nice 0
sudo ./engine start lo ./rootfs-beta  /cpu_workload 500000 --nice 10
# Wait for both to exit, then check logs:
sudo ./engine logs hi
sudo ./engine logs lo
```

**Interpretation:** The `[cpu_workload] found ... primes in X seconds` line in each log shows wall-clock completion time. With a weight ratio of ~9:1 (nice 0 vs nice 10), `hi` should finish roughly 9× faster than `lo` under contention. On a lightly loaded host the gap may be smaller because `lo` also runs when `hi` yields for I/O or sleep.

### Experiment B — CPU-bound vs I/O-bound at the same priority

| Container | Type    | nice | Wall time | CPU% observed |
|-----------|---------|------|-----------|---------------|
| cpu-c     | CPU     | 0    | _measured_ | ~100% |
| io-c      | I/O     | 0    | _measured_ | ~5–15% |

**How to reproduce:**
```bash
sudo ./engine start cpu-c ./rootfs-alpha /cpu_workload 500000 --nice 0
sudo ./engine start io-c  ./rootfs-beta  /io_workload  200    --nice 0
```

**Interpretation:** The I/O-bound container spends most of its time in disk wait (TASK_INTERRUPTIBLE), so it voluntarily yields the CPU. CFS accumulates a sleep credit for it (`min_vruntime` adjustment), meaning it gets a brief burst of CPU when it wakes for each write/read. The CPU-bound container consumes nearly all of the CPU time it is offered. This demonstrates CFS's I/O compensation: a process that sleeps does not fall arbitrarily far behind in vruntime, allowing it to remain responsive even under CPU competition.

---

_README template — fill in SRNs, measured timings, and screenshot paths before submission._
