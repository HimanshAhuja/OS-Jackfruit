/*
 * mem_workload.c — Memory stress test for kernel monitor experiments (Task 4)
 *
 * Allocates and touches memory in 1 MiB steps, sleeping 1 second between
 * each step so the kernel monitor's RSS check has time to fire.
 * Expects to be killed by SIGKILL from the kernel module once the hard
 * limit is crossed; the soft-limit warning should appear in dmesg first.
 *
 * Build: gcc -O2 -o mem_workload mem_workload.c
 * Usage: ./mem_workload [target_mib]   (default = 100 MiB)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int target = (argc > 1) ? atoi(argv[1]) : 100;
    size_t chunk = 1024 * 1024;   /* 1 MiB */

    printf("[mem_workload] pid=%d  will allocate up to %d MiB (1 MiB/sec)\n",
           getpid(), target);
    fflush(stdout);

    for (int i = 1; i <= target; i++) {
        char *p = malloc(chunk);
        if (!p) {
            fprintf(stderr, "[mem_workload] malloc failed at %d MiB\n", i);
            return 1;
        }
        /* Touch every page so the allocation shows up as RSS */
        memset(p, i & 0xFF, chunk);

        printf("[mem_workload] pid=%d  allocated %d MiB\n", getpid(), i);
        fflush(stdout);
        sleep(1);
    }

    printf("[mem_workload] pid=%d  reached %d MiB — sleeping\n",
           getpid(), target);
    fflush(stdout);
    while (1) sleep(10);
    return 0;
}
