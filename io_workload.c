/*
 * io_workload.c — I/O-bound workload for scheduler experiments (Task 5)
 *
 * Repeatedly writes and reads a temporary file to generate I/O wait.
 * This workload spends most of its time blocked on disk I/O, so the
 * Linux CFS scheduler should give CPU-bound containers more CPU time
 * when both are running concurrently.
 *
 * Build: gcc -O2 -o io_workload io_workload.c
 * Usage: ./io_workload [iterations]   (default = 200)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#define BUF_SIZE  (4 * 1024)        /* 4 KiB write buffer */
#define FILE_MiB  4                 /* write 4 MiB per iteration */
#define WRITES_PER_ITER (FILE_MiB * 1024 * 1024 / BUF_SIZE)

int main(int argc, char *argv[])
{
    int iterations = (argc > 1) ? atoi(argv[1]) : 200;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/io_workload_%d.tmp", getpid());

    char buf[BUF_SIZE];
    memset(buf, 0xAB, sizeof(buf));

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    printf("[io_workload] pid=%d  %d iterations × %d MiB write+read\n",
           getpid(), iterations, FILE_MiB);
    fflush(stdout);

    for (int i = 0; i < iterations; i++) {
        /* ── Write phase ── */
        FILE *f = fopen(path, "w");
        if (!f) { perror("fopen write"); return 1; }
        for (int j = 0; j < WRITES_PER_ITER; j++)
            fwrite(buf, 1, BUF_SIZE, f);
        fflush(f);
        fclose(f);

        /* ── Read phase ── */
        f = fopen(path, "r");
        if (!f) { perror("fopen read"); return 1; }
        while (fread(buf, 1, BUF_SIZE, f) > 0) { /* consume */ }
        fclose(f);

        if ((i + 1) % 20 == 0) {
            printf("[io_workload] pid=%d  iteration %d/%d\n",
                   getpid(), i + 1, iterations);
            fflush(stdout);
        }
    }

    gettimeofday(&t1, NULL);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_usec - t0.tv_usec) / 1e6;

    printf("[io_workload] pid=%d  done in %.3f seconds\n",
           getpid(), elapsed);
    fflush(stdout);

    unlink(path);   /* clean up temp file */
    return 0;
}
