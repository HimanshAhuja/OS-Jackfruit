/*
 * cpu_workload.c — CPU-bound workload for scheduler experiments (Task 5)
 *
 * Computes prime numbers up to a limit using trial division.
 * Prints timing information so the experiment can measure throughput
 * under different nice values and compare CPU share between containers.
 *
 * Build: gcc -O2 -o cpu_workload cpu_workload.c
 * Usage: ./cpu_workload [limit]   (default limit = 500000)
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

static long count_primes(long limit)
{
    long count = 0;
    for (long n = 2; n <= limit; n++) {
        int prime = 1;
        for (long d = 2; d * d <= n; d++) {
            if (n % d == 0) { prime = 0; break; }
        }
        if (prime) count++;
    }
    return count;
}

int main(int argc, char *argv[])
{
    long limit = (argc > 1) ? atol(argv[1]) : 500000L;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    printf("[cpu_workload] pid=%d  computing primes up to %ld ...\n",
           getpid(), limit);
    fflush(stdout);

    long primes = count_primes(limit);

    gettimeofday(&t1, NULL);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_usec - t0.tv_usec) / 1e6;

    printf("[cpu_workload] pid=%d  found %ld primes in %.3f seconds\n",
           getpid(), primes, elapsed);
    fflush(stdout);
    return 0;
}
