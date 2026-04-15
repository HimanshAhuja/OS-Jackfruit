/*
 * monitor.c — Container Memory Monitor (Linux Kernel Module)
 *
 * Build:   make (via Makefile)
 * Load:    sudo insmod monitor.ko
 * Verify:  ls -l /dev/container_monitor
 * Unload:  sudo rmmod monitor
 *
 * Behaviour:
 *   - Exposes /dev/container_monitor (character device)
 *   - Accepts CONTAINER_IOCTL_REGISTER   — track a container PID + limits
 *   - Accepts CONTAINER_IOCTL_UNREGISTER — remove a PID when it exits normally
 *   - A kernel timer fires every CHECK_INTERVAL_MS and checks RSS for each
 *     tracked PID:
 *       Soft limit breached (first time) → pr_warn (visible in dmesg)
 *       Hard limit breached              → SIGKILL + remove entry
 *   - Stale entries (process gone) are removed automatically
 *   - All list accesses are protected by a mutex (no spinlock: RSS check can
 *     sleep via get_task_mm / mmput)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Mini Project");
MODULE_DESCRIPTION("Container RSS memory monitor with soft and hard limits");
MODULE_VERSION("1.0");

/* ─────────────────────────────────────────────────────────────────
   Module parameters (tunable via modprobe / insmod)
   ───────────────────────────────────────────────────────────────── */
static int check_interval_ms = 1000;
module_param(check_interval_ms, int, 0444);
MODULE_PARM_DESC(check_interval_ms, "RSS check interval in milliseconds (default 1000)");

/* ─────────────────────────────────────────────────────────────────
   Per-container tracking entry
   ───────────────────────────────────────────────────────────────── */
struct container_entry {
    struct list_head list;
    pid_t            pid;
    int              soft_mib;
    int              hard_mib;
    int              soft_warned; /* 1 once the soft-limit warning is logged */
};

/* ─────────────────────────────────────────────────────────────────
   Global state
   ───────────────────────────────────────────────────────────────── */
static LIST_HEAD(container_list);
static DEFINE_MUTEX(container_mutex);
static struct timer_list rss_timer;
static dev_t             monitor_dev;
static struct cdev       monitor_cdev;
static struct class     *monitor_class;

/* ─────────────────────────────────────────────────────────────────
   RSS measurement (in MiB).  Returns -1 if the process is gone.
   ───────────────────────────────────────────────────────────────── */
static long get_rss_mib(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss = -1;

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) { rcu_read_unlock(); return -1; }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        /* get_mm_rss returns pages; convert to MiB */
        rss = (long)(get_mm_rss(mm) * PAGE_SIZE) >> 20;
        mmput(mm);
    }
    put_task_struct(task);
    return rss;
}

/* ─────────────────────────────────────────────────────────────────
   Send a signal to a process by PID
   ───────────────────────────────────────────────────────────────── */
static int signal_pid(pid_t pid, int sig)
{
    struct task_struct *task;
    int ret = -ESRCH;

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (task)
        ret = send_sig(sig, task, 1);
    rcu_read_unlock();
    return ret;
}

/* ─────────────────────────────────────────────────────────────────
   Timer callback — runs in softirq context, but we need to sleep
   for get_task_mm, so we hold the mutex (mutex_lock is allowed in
   timer callbacks on modern kernels when not in atomic context).
   We use a sleepable workqueue if needed, but a timer callback with
   mutex works for this use case.
   ───────────────────────────────────────────────────────────────── */
static void rss_check_callback(struct timer_list *t)
{
    struct container_entry *entry, *tmp;

    mutex_lock(&container_mutex);

    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        long rss = get_rss_mib(entry->pid);

        /* Process no longer exists → stale entry, remove it */
        if (rss < 0) {
            pr_info("container_monitor: pid %d gone, removing stale entry\n",
                    entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Hard limit — kill and remove */
        if (rss >= entry->hard_mib) {
            pr_warn("container_monitor: [HARD LIMIT] pid=%d rss=%ldMiB "
                    ">= hard=%dMiB — sending SIGKILL\n",
                    entry->pid, rss, entry->hard_mib);
            signal_pid(entry->pid, SIGKILL);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit — warn once */
        if (rss >= entry->soft_mib && !entry->soft_warned) {
            pr_warn("container_monitor: [SOFT LIMIT] pid=%d rss=%ldMiB "
                    ">= soft=%dMiB — warning (hard=%dMiB)\n",
                    entry->pid, rss, entry->soft_mib, entry->hard_mib);
            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&container_mutex);

    /* Re-arm the timer */
    mod_timer(&rss_timer,
              jiffies + msecs_to_jiffies((unsigned int)check_interval_ms));
}

/* ─────────────────────────────────────────────────────────────────
   ioctl handler
   ───────────────────────────────────────────────────────────────── */
static long monitor_ioctl(struct file *filp,
                           unsigned int  cmd,
                           unsigned long arg)
{
    (void)filp;

    switch (cmd) {

    /* ── Register a new container ── */
    case CONTAINER_IOCTL_REGISTER: {
        struct container_reg reg;
        if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
            return -EFAULT;
        if (reg.soft_mib <= 0 || reg.hard_mib <= 0 ||
            reg.soft_mib >= reg.hard_mib) {
            pr_err("container_monitor: invalid limits soft=%d hard=%d\n",
                   reg.soft_mib, reg.hard_mib);
            return -EINVAL;
        }

        struct container_entry *e = kmalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;

        e->pid        = (pid_t)reg.pid;
        e->soft_mib   = reg.soft_mib;
        e->hard_mib   = reg.hard_mib;
        e->soft_warned = 0;
        INIT_LIST_HEAD(&e->list);

        mutex_lock(&container_mutex);
        list_add_tail(&e->list, &container_list);
        mutex_unlock(&container_mutex);

        pr_info("container_monitor: registered pid=%d soft=%dMiB hard=%dMiB\n",
                e->pid, e->soft_mib, e->hard_mib);
        return 0;
    }

    /* ── Unregister a container (normal exit path) ── */
    case CONTAINER_IOCTL_UNREGISTER: {
        __s32 pid_val;
        if (copy_from_user(&pid_val, (void __user *)arg, sizeof(pid_val)))
            return -EFAULT;

        pid_t target = (pid_t)pid_val;
        mutex_lock(&container_mutex);
        struct container_entry *e, *tmp;
        list_for_each_entry_safe(e, tmp, &container_list, list) {
            if (e->pid == target) {
                list_del(&e->list);
                kfree(e);
                pr_info("container_monitor: unregistered pid=%d\n", target);
                break;
            }
        }
        mutex_unlock(&container_mutex);
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

/* ─────────────────────────────────────────────────────────────────
   File operations
   ───────────────────────────────────────────────────────────────── */
static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ─────────────────────────────────────────────────────────────────
   Module init
   ───────────────────────────────────────────────────────────────── */
static int __init monitor_init(void)
{
    int ret;

    /* Allocate a major number dynamically */
    ret = alloc_chrdev_region(&monitor_dev, 0, 1, "container_monitor");
    if (ret < 0) {
        pr_err("container_monitor: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    /* Initialise and add character device */
    cdev_init(&monitor_cdev, &monitor_fops);
    monitor_cdev.owner = THIS_MODULE;
    ret = cdev_add(&monitor_cdev, monitor_dev, 1);
    if (ret < 0) {
        pr_err("container_monitor: cdev_add failed: %d\n", ret);
        goto err_cdev;
    }

    /* Create /dev/container_monitor automatically via udev */
    monitor_class = class_create(THIS_MODULE, "container_monitor");
    if (IS_ERR(monitor_class)) {
        ret = PTR_ERR(monitor_class);
        pr_err("container_monitor: class_create failed: %d\n", ret);
        goto err_class;
    }
    device_create(monitor_class, NULL, monitor_dev, NULL, "container_monitor");

    /* Start RSS check timer */
    timer_setup(&rss_timer, rss_check_callback, 0);
    mod_timer(&rss_timer,
              jiffies + msecs_to_jiffies((unsigned int)check_interval_ms));

    pr_info("container_monitor: loaded (major=%d, interval=%dms)\n",
            MAJOR(monitor_dev), check_interval_ms);
    return 0;

err_class:
    cdev_del(&monitor_cdev);
err_cdev:
    unregister_chrdev_region(monitor_dev, 1);
    return ret;
}

/* ─────────────────────────────────────────────────────────────────
   Module exit — clean up kernel list, timer, and device
   ───────────────────────────────────────────────────────────────── */
static void __exit monitor_exit(void)
{
    /* Stop timer first so no more callbacks fire */
    del_timer_sync(&rss_timer);

    /* Free all tracked entries */
    mutex_lock(&container_mutex);
    struct container_entry *e, *tmp;
    list_for_each_entry_safe(e, tmp, &container_list, list) {
        list_del(&e->list);
        kfree(e);
    }
    mutex_unlock(&container_mutex);

    /* Destroy device and character device */
    device_destroy(monitor_class, monitor_dev);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(monitor_dev, 1);

    pr_info("container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
