#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

/*
 * monitor_ioctl.h — shared definitions between engine.c (user space)
 * and monitor.c (kernel module).
 *
 * Included by both sides; uses <linux/ioctl.h> which is safe for
 * user-space inclusion when _GNU_SOURCE is defined.
 */

#ifdef __KERNEL__
#  include <linux/ioctl.h>
#  include <linux/types.h>
#else
#  include <sys/ioctl.h>
#  include <stdint.h>
   typedef int32_t __s32;
#endif

#define MONITOR_MAGIC 'M'

/* Payload sent when registering a container process */
struct container_reg {
    __s32 pid;       /* host PID of the container init process */
    __s32 soft_mib;  /* RSS soft-limit in MiB — log warning on first breach */
    __s32 hard_mib;  /* RSS hard-limit in MiB — SIGKILL on breach           */
};

/*
 * CONTAINER_IOCTL_REGISTER   — register a new container PID + limits
 * CONTAINER_IOCTL_UNREGISTER — remove a container by PID (normal exit path)
 */
#define CONTAINER_IOCTL_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_reg)
#define CONTAINER_IOCTL_UNREGISTER _IOW(MONITOR_MAGIC, 2, __s32)

#endif /* MONITOR_IOCTL_H */
