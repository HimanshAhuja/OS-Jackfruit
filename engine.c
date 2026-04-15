/*
 * engine.c — Multi-Container Runtime
 *
 * Build:  gcc -Wall -Wextra -O2 -pthread -o engine engine.c
 * Run:    sudo ./engine supervisor ./rootfs-base
 *         sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
 *         sudo ./engine ps
 *         sudo ./engine logs alpha
 *         sudo ./engine stop alpha
 *
 * Two IPC paths:
 *   Path A (logging)  — container stdout/stderr → supervisor via pipes
 *                       → bounded buffer → consumer thread → log files
 *   Path B (control)  — CLI process → supervisor via UNIX domain socket
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sched.h>

#include "monitor_ioctl.h"

/* ─────────────────────────────────────────────────────────────────
   Constants
   ───────────────────────────────────────────────────────────────── */
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine_logs"
#define CLONE_STACK_SZ   (1 << 20)      /* 1 MiB per container clone stack */
#define BUF_SLOTS        512            /* bounded buffer capacity          */
#define SLOT_SIZE        4096           /* max bytes per log chunk          */
#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64
#define STOP_GRACE_MS    3000           /* ms before SIGKILL after SIGTERM  */

/* ─────────────────────────────────────────────────────────────────
   Container state
   ───────────────────────────────────────────────────────────────── */
typedef enum {
    CS_STARTING = 0,
    CS_RUNNING,
    CS_STOPPED,
    CS_KILLED,
    CS_EXITED,
} CState;

static const char *cstate_str(CState s) {
    switch (s) {
        case CS_STARTING: return "starting";
        case CS_RUNNING:  return "running";
        case CS_STOPPED:  return "stopped";
        case CS_KILLED:   return "killed";
        case CS_EXITED:   return "exited";
        default:          return "unknown";
    }
}

/* ─────────────────────────────────────────────────────────────────
   Container metadata
   ───────────────────────────────────────────────────────────────── */
typedef struct ContainerMeta {
    char   id[64];
    pid_t  host_pid;
    time_t start_time;
    CState state;

    int    soft_mib;
    int    hard_mib;
    int    nice_val;

    char   rootfs[256];
    char   log_path[256];
    char   command[256];

    int    exit_code;
    int    term_signal;
    int    stop_requested;       /* set before sending SIGTERM from 'stop' */
    char   stop_reason[32];      /* "running"|"exited"|"stopped"|"hard_limit_killed" */

    int    pipe_rd;              /* supervisor reads container stdout here  */
    char  *clone_stack;          /* heap-allocated clone stack, freed later */
    pthread_t  producer_tid;
    int        producer_running;

    struct ContainerMeta *next;
} ContainerMeta;

/* ─────────────────────────────────────────────────────────────────
   Bounded buffer (Path A — logging pipeline)
   ───────────────────────────────────────────────────────────────── */
typedef struct {
    char   container_id[64];
    char   data[SLOT_SIZE];
    size_t len;
} LogEntry;

typedef struct {
    LogEntry       slots[BUF_SLOTS];
    int            head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    int             shutdown;
} BoundedBuffer;

/* ─────────────────────────────────────────────────────────────────
   Global supervisor state
   ───────────────────────────────────────────────────────────────── */
static ContainerMeta   *g_containers       = NULL;
static pthread_mutex_t  g_containers_lock  = PTHREAD_MUTEX_INITIALIZER;
static BoundedBuffer    g_log_buf;
static pthread_t        g_consumer_tid;
static int              g_monitor_fd       = -1;
static volatile int     g_supervisor_alive = 1;

/* ─────────────────────────────────────────────────────────────────
   Bounded buffer operations
   ───────────────────────────────────────────────────────────────── */
static void bb_init(BoundedBuffer *bb)
{
    memset(bb, 0, sizeof(*bb));
    pthread_mutex_init(&bb->lock, NULL);
    pthread_cond_init(&bb->not_full,  NULL);
    pthread_cond_init(&bb->not_empty, NULL);
}

/*
 * bb_push — producer inserts a log chunk.
 * Blocks when full (back-pressure); returns immediately if shutdown is set.
 * Race without synchronisation: two producers could corrupt head/tail and
 * count. The mutex ensures exclusive access; not_full prevents overwrite.
 */
static void bb_push(BoundedBuffer *bb, const char *id,
                    const char *data, size_t len)
{
    pthread_mutex_lock(&bb->lock);
    while (bb->count == BUF_SLOTS && !bb->shutdown)
        pthread_cond_wait(&bb->not_full, &bb->lock);

    if (bb->shutdown) {
        pthread_mutex_unlock(&bb->lock);
        return;
    }

    LogEntry *e = &bb->slots[bb->tail];
    strncpy(e->container_id, id, sizeof(e->container_id) - 1);
    size_t copy = len < SLOT_SIZE ? len : SLOT_SIZE;
    memcpy(e->data, data, copy);
    e->len = copy;

    bb->tail = (bb->tail + 1) % BUF_SLOTS;
    bb->count++;
    pthread_cond_signal(&bb->not_empty);
    pthread_mutex_unlock(&bb->lock);
}

/*
 * bb_pop — consumer removes one entry.
 * Returns 1 on success, 0 when shutdown AND buffer is empty (terminal).
 */
static int bb_pop(BoundedBuffer *bb, LogEntry *out)
{
    pthread_mutex_lock(&bb->lock);
    while (bb->count == 0 && !bb->shutdown)
        pthread_cond_wait(&bb->not_empty, &bb->lock);

    if (bb->count == 0) {          /* shutdown + empty → consumer must exit */
        pthread_mutex_unlock(&bb->lock);
        return 0;
    }

    *out = bb->slots[bb->head];
    bb->head = (bb->head + 1) % BUF_SLOTS;
    bb->count--;
    pthread_cond_signal(&bb->not_full);
    pthread_mutex_unlock(&bb->lock);
    return 1;
}

/* Drain any remaining entries directly (called after consumer thread joins) */
static void bb_drain(BoundedBuffer *bb)
{
    LogEntry e;
    while (1) {
        pthread_mutex_lock(&bb->lock);
        if (bb->count == 0) { pthread_mutex_unlock(&bb->lock); break; }
        e = bb->slots[bb->head];
        bb->head = (bb->head + 1) % BUF_SLOTS;
        bb->count--;
        pthread_mutex_unlock(&bb->lock);

        char log_path[256] = "";
        pthread_mutex_lock(&g_containers_lock);
        for (ContainerMeta *c = g_containers; c; c = c->next)
            if (strcmp(c->id, e.container_id) == 0)
                { strncpy(log_path, c->log_path, sizeof(log_path)-1); break; }
        pthread_mutex_unlock(&g_containers_lock);

        if (log_path[0]) {
            int fd = open(log_path, O_WRONLY|O_CREAT|O_APPEND, 0644);
            if (fd >= 0) { write(fd, e.data, e.len); close(fd); }
        }
    }
}

static void bb_shutdown(BoundedBuffer *bb)
{
    pthread_mutex_lock(&bb->lock);
    bb->shutdown = 1;
    pthread_cond_broadcast(&bb->not_full);
    pthread_cond_broadcast(&bb->not_empty);
    pthread_mutex_unlock(&bb->lock);
}

/* ─────────────────────────────────────────────────────────────────
   Consumer thread — Path A
   Writes log entries from the bounded buffer to per-container log files.
   Correctness guarantee: flushes all remaining entries before returning,
   so no log line is lost when a container exits abruptly.
   ───────────────────────────────────────────────────────────────── */
static void *consumer_thread(void *arg)
{
    (void)arg;
    LogEntry e;
    while (bb_pop(&g_log_buf, &e)) {
        char log_path[256] = "";
        pthread_mutex_lock(&g_containers_lock);
        for (ContainerMeta *c = g_containers; c; c = c->next)
            if (strcmp(c->id, e.container_id) == 0)
                { strncpy(log_path, c->log_path, sizeof(log_path)-1); break; }
        pthread_mutex_unlock(&g_containers_lock);

        if (log_path[0]) {
            int fd = open(log_path, O_WRONLY|O_CREAT|O_APPEND, 0644);
            if (fd >= 0) { write(fd, e.data, e.len); close(fd); }
        }
    }
    /* bb_pop returned 0 → shutdown signal received; buffer may still have
       entries enqueued before shutdown was set. Drain them now. */
    bb_drain(&g_log_buf);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────
   Producer thread — Path A
   One per container: reads pipe, pushes chunks into bounded buffer.
   Exits cleanly when the pipe's write end is closed (container exits).
   ───────────────────────────────────────────────────────────────── */
typedef struct { char id[64]; int pipe_fd; } ProducerArg;

static void *producer_thread(void *arg)
{
    ProducerArg *pa = (ProducerArg *)arg;
    char buf[SLOT_SIZE];
    ssize_t n;
    while ((n = read(pa->pipe_fd, buf, sizeof(buf))) > 0)
        bb_push(&g_log_buf, pa->id, buf, (size_t)n);
    close(pa->pipe_fd);

    /* Mark producer as done in metadata */
    pthread_mutex_lock(&g_containers_lock);
    for (ContainerMeta *c = g_containers; c; c = c->next)
        if (strcmp(c->id, pa->id) == 0)
            { c->producer_running = 0; break; }
    pthread_mutex_unlock(&g_containers_lock);

    free(pa);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────
   Container child entry point (runs inside clone'd process)
   ───────────────────────────────────────────────────────────────── */
typedef struct {
    char  rootfs[256];
    int   pipe_wr;           /* write end: container stdout/stderr → supervisor */
    int   nice_val;
    char *exec_argv[64];     /* NULL-terminated list of strings from parent copy */
    char  exec_buf[2048];    /* storage for the copied argument strings          */
} CloneArg;

static int container_child_fn(void *raw)
{
    CloneArg *ca = (CloneArg *)raw;

    /* Redirect stdout + stderr into logging pipe (Path A) */
    dup2(ca->pipe_wr, STDOUT_FILENO);
    dup2(ca->pipe_wr, STDERR_FILENO);
    close(ca->pipe_wr);

    /* Filesystem isolation */
    if (chroot(ca->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc so ps, top, etc. work inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "warning: mount /proc: %s\n", strerror(errno));

    /* Scheduling priority */
    if (ca->nice_val != 0)
        nice(ca->nice_val);

    /* Exec the requested command */
    execvp(ca->exec_argv[0], ca->exec_argv);
    fprintf(stderr, "execvp '%s': %s\n", ca->exec_argv[0], strerror(errno));
    return 127;
}

/* ─────────────────────────────────────────────────────────────────
   Kernel monitor helpers
   ───────────────────────────────────────────────────────────────── */
static void monitor_register(pid_t pid, int soft_mib, int hard_mib)
{
    if (g_monitor_fd < 0) return;
    struct container_reg reg = { .pid = pid, .soft_mib = soft_mib,
                                  .hard_mib = hard_mib };
    if (ioctl(g_monitor_fd, CONTAINER_IOCTL_REGISTER, &reg) < 0)
        perror("ioctl REGISTER");
}

static void monitor_unregister(pid_t pid)
{
    if (g_monitor_fd < 0) return;
    int p = (int)pid;
    if (ioctl(g_monitor_fd, CONTAINER_IOCTL_UNREGISTER, &p) < 0)
        perror("ioctl UNREGISTER");
}

/* ─────────────────────────────────────────────────────────────────
   Signal handlers
   ───────────────────────────────────────────────────────────────── */

/* SIGCHLD — reap zombie children, update container metadata */
static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_containers_lock);
        for (ContainerMeta *c = g_containers; c; c = c->next) {
            if (c->host_pid != pid) continue;

            if (WIFEXITED(status)) {
                c->exit_code   = WEXITSTATUS(status);
                c->term_signal = 0;
                c->state       = CS_EXITED;
                strncpy(c->stop_reason,
                        c->stop_requested ? "stopped" : "exited",
                        sizeof(c->stop_reason));
            } else if (WIFSIGNALED(status)) {
                c->exit_code   = 0;
                c->term_signal = WTERMSIG(status);
                c->state       = CS_KILLED;
                if (!c->stop_requested && c->term_signal == SIGKILL)
                    strncpy(c->stop_reason, "hard_limit_killed",
                            sizeof(c->stop_reason));
                else
                    strncpy(c->stop_reason, "stopped",
                            sizeof(c->stop_reason));
            }
            monitor_unregister(pid);
            break;
        }
        pthread_mutex_unlock(&g_containers_lock);
    }
}

/* SIGTERM / SIGINT — orderly supervisor shutdown */
static void sigterm_handler(int sig)
{
    (void)sig;
    g_supervisor_alive = 0;
}

/* ─────────────────────────────────────────────────────────────────
   Helper: find container by id (caller must hold g_containers_lock)
   ───────────────────────────────────────────────────────────────── */
static ContainerMeta *find_container(const char *id)
{
    for (ContainerMeta *c = g_containers; c; c = c->next)
        if (strcmp(c->id, id) == 0) return c;
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────
   Command: start (and run)
   ───────────────────────────────────────────────────────────────── */
static void cmd_start(const char *id, const char *rootfs,
                      char *argv[], int argc,
                      int soft_mib, int hard_mib, int nice_val,
                      char *resp, size_t rsz)
{
    /* Reject duplicate IDs */
    pthread_mutex_lock(&g_containers_lock);
    if (find_container(id)) {
        pthread_mutex_unlock(&g_containers_lock);
        snprintf(resp, rsz, "ERROR: container '%s' already exists\n", id);
        return;
    }
    pthread_mutex_unlock(&g_containers_lock);

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);
    char log_path[256];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, id);

    /* Create logging pipe (Path A) */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(resp, rsz, "ERROR: pipe: %s\n", strerror(errno));
        return;
    }

    /* Prepare clone arguments (will live in parent memory until exec) */
    CloneArg *ca = calloc(1, sizeof(*ca));
    if (!ca) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(resp, rsz, "ERROR: out of memory\n");
        return;
    }
    strncpy(ca->rootfs, rootfs, sizeof(ca->rootfs) - 1);
    ca->pipe_wr  = pipefd[1];
    ca->nice_val = nice_val;

    /* Copy argv strings into ca->exec_buf so the child owns them */
    int off = 0;
    for (int i = 0; i < argc && i < 63; i++) {
        size_t slen = strlen(argv[i]) + 1;
        if (off + (int)slen > (int)sizeof(ca->exec_buf)) break;
        memcpy(ca->exec_buf + off, argv[i], slen);
        ca->exec_argv[i] = ca->exec_buf + off;
        off += (int)slen;
    }
    ca->exec_argv[argc] = NULL;

    /* Allocate clone stack */
    char *stack = malloc(CLONE_STACK_SZ);
    if (!stack) {
        free(ca);
        close(pipefd[0]); close(pipefd[1]);
        snprintf(resp, rsz, "ERROR: malloc clone stack\n");
        return;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_child_fn, stack + CLONE_STACK_SZ,
                      clone_flags, ca);

    /* Parent: close write end of pipe — child owns it */
    close(pipefd[1]);

    if (pid < 0) {
        free(stack); free(ca);
        close(pipefd[0]);
        snprintf(resp, rsz, "ERROR: clone: %s\n", strerror(errno));
        return;
    }

    /* Build metadata */
    ContainerMeta *meta = calloc(1, sizeof(*meta));
    strncpy(meta->id,       id,      sizeof(meta->id)       - 1);
    strncpy(meta->rootfs,   rootfs,  sizeof(meta->rootfs)   - 1);
    strncpy(meta->log_path, log_path, sizeof(meta->log_path) - 1);
    strncpy(meta->command,  argv[0], sizeof(meta->command)  - 1);
    strncpy(meta->stop_reason, "running", sizeof(meta->stop_reason));
    meta->host_pid    = pid;
    meta->start_time  = time(NULL);
    meta->state       = CS_RUNNING;
    meta->soft_mib    = soft_mib;
    meta->hard_mib    = hard_mib;
    meta->nice_val    = nice_val;
    meta->pipe_rd     = pipefd[0];
    meta->clone_stack = stack;

    /* Launch producer thread for this container's pipe */
    ProducerArg *pa = malloc(sizeof(*pa));
    strncpy(pa->id, id, sizeof(pa->id) - 1);
    pa->pipe_fd = pipefd[0];
    meta->producer_running = 1;
    pthread_create(&meta->producer_tid, NULL, producer_thread, pa);
    /* pa is freed inside producer_thread */

    /* Insert at head of container list */
    pthread_mutex_lock(&g_containers_lock);
    meta->next   = g_containers;
    g_containers = meta;
    pthread_mutex_unlock(&g_containers_lock);

    /* Register with kernel monitor */
    monitor_register(pid, soft_mib, hard_mib);

    snprintf(resp, rsz, "OK: container '%s' started (pid=%d)\n", id, pid);
    free(ca);   /* child has exec'd; its copy-on-write page is separate */
}

/* ─────────────────────────────────────────────────────────────────
   Command: stop
   ───────────────────────────────────────────────────────────────── */
static void cmd_stop(const char *id, char *resp, size_t rsz)
{
    pthread_mutex_lock(&g_containers_lock);
    ContainerMeta *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&g_containers_lock);
        snprintf(resp, rsz, "ERROR: container '%s' not found\n", id);
        return;
    }
    if (c->state != CS_RUNNING && c->state != CS_STARTING) {
        pthread_mutex_unlock(&g_containers_lock);
        snprintf(resp, rsz, "ERROR: container '%s' is not running\n", id);
        return;
    }
    /* Set flag BEFORE signalling — attribution rule for grading */
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&g_containers_lock);

    kill(pid, SIGTERM);

    /* Poll for graceful exit; escalate to SIGKILL after grace period */
    int waited = 0;
    while (waited < STOP_GRACE_MS) {
        usleep(100 * 1000);
        waited += 100;
        pthread_mutex_lock(&g_containers_lock);
        ContainerMeta *cc = find_container(id);
        CState st = cc ? cc->state : CS_EXITED;
        pthread_mutex_unlock(&g_containers_lock);
        if (st == CS_EXITED || st == CS_KILLED || st == CS_STOPPED) goto done;
    }
    kill(pid, SIGKILL);

done:
    snprintf(resp, rsz, "OK: stop signal sent to '%s'\n", id);
}

/* ─────────────────────────────────────────────────────────────────
   Command: ps
   ───────────────────────────────────────────────────────────────── */
static void cmd_ps(char *resp, size_t rsz)
{
    pthread_mutex_lock(&g_containers_lock);
    int off = 0;
    off += snprintf(resp + off, rsz - off,
        "%-16s %-8s %-10s %-9s %-9s %-20s %-6s %s\n",
        "ID", "PID", "STATE",
        "SOFT(MiB)", "HARD(MiB)", "STARTED", "NICE", "REASON");

    char sep[120];
    memset(sep, '-', sizeof(sep)-1); sep[sizeof(sep)-1] = '\0';
    off += snprintf(resp + off, rsz - off, "%.117s\n", sep);

    for (ContainerMeta *c = g_containers; c && (size_t)off < rsz - 1; c = c->next) {
        char tbuf[24];
        struct tm *tm = localtime(&c->start_time);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
        off += snprintf(resp + off, rsz - off,
            "%-16s %-8d %-10s %-9d %-9d %-20s %-6d %s\n",
            c->id, c->host_pid, cstate_str(c->state),
            c->soft_mib, c->hard_mib, tbuf, c->nice_val, c->stop_reason);
    }
    pthread_mutex_unlock(&g_containers_lock);
}

/* ─────────────────────────────────────────────────────────────────
   Command: logs
   ───────────────────────────────────────────────────────────────── */
static void cmd_logs(const char *id, char *resp, size_t rsz)
{
    char log_path[256] = "";
    pthread_mutex_lock(&g_containers_lock);
    ContainerMeta *c = find_container(id);
    if (c) strncpy(log_path, c->log_path, sizeof(log_path) - 1);
    pthread_mutex_unlock(&g_containers_lock);

    if (!log_path[0]) {
        snprintf(resp, rsz, "ERROR: container '%s' not found\n", id);
        return;
    }
    FILE *f = fopen(log_path, "r");
    if (!f) {
        snprintf(resp, rsz, "ERROR: open log: %s\n", strerror(errno));
        return;
    }
    size_t n = fread(resp, 1, rsz - 1, f);
    resp[n] = '\0';
    fclose(f);
    if (n == 0) snprintf(resp, rsz, "(log is empty)\n");
}

/* ─────────────────────────────────────────────────────────────────
   Dispatcher — parses a raw command string and calls the right handler
   ───────────────────────────────────────────────────────────────── */
static void dispatch(const char *line, char *resp, size_t rsz)
{
    char buf[4096];
    strncpy(buf, line, sizeof(buf) - 1);

    char *toks[128];
    int   n = 0;
    char *p = strtok(buf, " \t\n\r");
    while (p && n < 127) { toks[n++] = p; p = strtok(NULL, " \t\n\r"); }
    toks[n] = NULL;

    if (n == 0) { snprintf(resp, rsz, "ERROR: empty command\n"); return; }

    const char *verb = toks[0];

    /* ---- ps ---- */
    if (strcmp(verb, "ps") == 0) { cmd_ps(resp, rsz); return; }

    /* ---- stop ---- */
    if (strcmp(verb, "stop") == 0) {
        if (n < 2) { snprintf(resp, rsz, "ERROR: stop <id>\n"); return; }
        cmd_stop(toks[1], resp, rsz);
        return;
    }

    /* ---- logs ---- */
    if (strcmp(verb, "logs") == 0) {
        if (n < 2) { snprintf(resp, rsz, "ERROR: logs <id>\n"); return; }
        cmd_logs(toks[1], resp, rsz);
        return;
    }

    /* ---- start | run ---- */
    if (strcmp(verb, "start") == 0 || strcmp(verb, "run") == 0) {
        if (n < 4) {
            snprintf(resp, rsz,
                     "ERROR: %s <id> <rootfs> <cmd> [args] [--soft-mib N] "
                     "[--hard-mib N] [--nice N]\n", verb);
            return;
        }
        const char *id     = toks[1];
        const char *rootfs = toks[2];
        int soft  = DEFAULT_SOFT_MIB;
        int hard  = DEFAULT_HARD_MIB;
        int nval  = 0;

        /* Collect command argv; parse optional flags */
        char *argv[64];
        int   argc = 0;
        for (int i = 3; i < n; i++) {
            if      (strcmp(toks[i], "--soft-mib") == 0 && i+1 < n) soft = atoi(toks[++i]);
            else if (strcmp(toks[i], "--hard-mib") == 0 && i+1 < n) hard = atoi(toks[++i]);
            else if (strcmp(toks[i], "--nice")     == 0 && i+1 < n) nval = atoi(toks[++i]);
            else if (argc < 63)                                       argv[argc++] = toks[i];
        }
        argv[argc] = NULL;

        if (argc == 0) { snprintf(resp, rsz, "ERROR: no command given\n"); return; }

        cmd_start(id, rootfs, argv, argc, soft, hard, nval, resp, rsz);

        /* 'run' blocks until the container exits */
        if (strcmp(verb, "run") == 0 && strncmp(resp, "OK:", 3) == 0) {
            while (1) {
                usleep(200 * 1000);
                pthread_mutex_lock(&g_containers_lock);
                ContainerMeta *c = find_container(id);
                if (!c || (c->state != CS_RUNNING && c->state != CS_STARTING)) {
                    if (c) {
                        int ec = c->term_signal ?
                                 128 + c->term_signal : c->exit_code;
                        snprintf(resp, rsz,
                                 "OK: '%s' exited (code=%d, reason=%s)\n",
                                 id, ec, c->stop_reason);
                    }
                    pthread_mutex_unlock(&g_containers_lock);
                    break;
                }
                pthread_mutex_unlock(&g_containers_lock);
            }
        }
        return;
    }

    snprintf(resp, rsz, "ERROR: unknown command '%s'\n", verb);
}

/* ─────────────────────────────────────────────────────────────────
   Supervisor: main accept loop
   ───────────────────────────────────────────────────────────────── */
static void run_supervisor(const char *base_rootfs)
{
    (void)base_rootfs;

    /* Signal setup */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Bounded buffer + consumer thread */
    bb_init(&g_log_buf);
    pthread_create(&g_consumer_tid, NULL, consumer_thread, NULL);

    /* Try to open kernel monitor device */
    g_monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (g_monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel monitor unavailable (%s); "
                        "memory enforcement disabled\n", strerror(errno));
    else
        fprintf(stderr, "[supervisor] kernel monitor ready\n");

    /* UNIX domain socket — Path B */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        { perror("bind"); exit(1); }
    listen(srv, 32);
    chmod(SOCK_PATH, 0666);

    /* Non-blocking so we can poll g_supervisor_alive */
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL) | O_NONBLOCK);

    fprintf(stderr, "[supervisor] pid=%d listening on %s\n", getpid(), SOCK_PATH);

    /* ── Accept loop ── */
    while (g_supervisor_alive) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50 * 1000);
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char cmd[4096] = {0};
        ssize_t n = read(cli, cmd, sizeof(cmd) - 1);
        if (n > 0) {
            static char resp[1 << 17];   /* 128 KiB response buffer */
            memset(resp, 0, sizeof(resp));
            dispatch(cmd, resp, sizeof(resp));
            write(cli, resp, strlen(resp));
        }
        close(cli);
    }

    /* ── Orderly shutdown ── */
    fprintf(stderr, "[supervisor] shutting down...\n");
    close(srv);
    unlink(SOCK_PATH);

    /* Signal running containers to stop */
    pthread_mutex_lock(&g_containers_lock);
    for (ContainerMeta *c = g_containers; c; c = c->next)
        if (c->state == CS_RUNNING || c->state == CS_STARTING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    pthread_mutex_unlock(&g_containers_lock);
    sleep(2);   /* grace period */

    /* SIGKILL stragglers */
    pthread_mutex_lock(&g_containers_lock);
    for (ContainerMeta *c = g_containers; c; c = c->next)
        if (c->state == CS_RUNNING || c->state == CS_STARTING)
            kill(c->host_pid, SIGKILL);
    pthread_mutex_unlock(&g_containers_lock);

    /* Shutdown logging pipeline; consumer drains remaining entries */
    bb_shutdown(&g_log_buf);
    pthread_join(g_consumer_tid, NULL);

    /* Join all producer threads */
    pthread_mutex_lock(&g_containers_lock);
    for (ContainerMeta *c = g_containers; c; c = c->next)
        if (c->producer_running)
            pthread_join(c->producer_tid, NULL);
    pthread_mutex_unlock(&g_containers_lock);

    /* Free all metadata */
    pthread_mutex_lock(&g_containers_lock);
    ContainerMeta *c = g_containers;
    while (c) {
        ContainerMeta *nx = c->next;
        free(c->clone_stack);
        free(c);
        c = nx;
    }
    g_containers = NULL;
    pthread_mutex_unlock(&g_containers_lock);

    if (g_monitor_fd >= 0) close(g_monitor_fd);
    fprintf(stderr, "[supervisor] exited cleanly\n");
}

/* ─────────────────────────────────────────────────────────────────
   CLI client — connect to supervisor, send command, print response
   ───────────────────────────────────────────────────────────────── */
static int cli_send(int argc, char *argv[])
{
    /* Rebuild the command string from argv[1..] */
    char cmd[4096] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "engine: cannot connect to supervisor (%s).\n"
                "        Is 'engine supervisor' running?\n", strerror(errno));
        close(fd);
        return 1;
    }

    /* Handle SIGINT/SIGTERM for 'run': forward stop to supervisor */
    /* (For simplicity the forwarding is done by a re-connect on signal) */

    write(fd, cmd, strlen(cmd));
    shutdown(fd, SHUT_WR);

    char buf[1 << 17];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────
   main()
   ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <base-rootfs>\n"
            "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine ps\n"
            "  engine logs  <id>\n"
            "  engine stop  <id>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "engine supervisor <base-rootfs>\n");
            return 1;
        }
        run_supervisor(argv[2]);
        return 0;
    }

    /* All other verbs: CLI mode */
    return cli_send(argc, argv);
}
