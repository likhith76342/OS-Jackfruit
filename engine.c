/*
 * engine.c - Supervised Multi-Container Runtime
 *
 * FIXES APPLIED:
 *  1. Added forward declarations for sigchld_handler, sigterm_handler,
 *     logging_thread, unregister_from_monitor so they can be referenced
 *     before their definitions.
 *  2. Renamed all bb_init / bb_pop / bb_push / bb_shutdown / bb_destroy
 *     call-sites to match the actual function names:
 *       bb_init     -> bounded_buffer_init
 *       bb_pop      -> bounded_buffer_pop
 *       bb_push     -> bounded_buffer_push
 *       bb_shutdown -> bounded_buffer_begin_shutdown
 *       bb_destroy  -> bounded_buffer_destroy
 *  3. Removed the stray '/' character before handle_request (was a
 *     syntax error that would prevent compilation).
 *  4. Fixed the broken return statement in send_control_request():
 *       "return \n if(...) return 0; else return 1;"
 *     replaced with a single correct conditional return.
 *  5. Made unregister_from_monitor static for consistency.
 *  6. Fixed snprintf size_t/int sign-mismatch cast in CMD_PS handler.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "monitor_ioctl.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 512
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)

/* =========================================================================
 * Enumerations
 * ========================================================================= */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef enum {
    TERM_NONE = 0,
    TERM_EXITED,
    TERM_STOPPED,
    TERM_HARD_LIMIT
} term_reason_t;

/* =========================================================================
 * Data structures
 * ========================================================================= */

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    term_reason_t term_reason;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head, tail, count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_code;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int pipe_fd;
} child_config_t;

typedef struct {
    int pipe_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} producer_arg_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global pointer used by signal handlers (they cannot receive arguments) */
static supervisor_ctx_t *global_ctx = NULL;

/* =========================================================================
 * FIX 1: Forward declarations
 *   sigchld_handler and sigterm_handler are referenced inside run_supervisor
 *   (via sigaction) but defined later in the file.  Without these forward
 *   declarations the compiler would error on the implicit use.
 *   logging_thread is referenced in pthread_create inside run_supervisor.
 *   unregister_from_monitor is called from sigchld_handler.
 * ========================================================================= */
static void sigchld_handler(int sig);
static void sigterm_handler(int sig);
static void *logging_thread(void *arg);
static int  unregister_from_monitor(int monitor_fd,
                                    const char *container_id,
                                    pid_t host_pid);

/* =========================================================================
 * Usage / argument helpers
 * ========================================================================= */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                           const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                 char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        long nice_value;
        char *end = NULL;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1],
                               &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1],
                               &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid --nice: %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* =========================================================================
 * String helpers
 * ========================================================================= */

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static const char *term_reason_string(term_reason_t r)
{
    switch (r) {
    case TERM_STOPPED:    return "stopped";
    case TERM_HARD_LIMIT: return "hard_limit_killed";
    case TERM_EXITED:     return "exited";
    default:              return "none";
    }
}

/* =========================================================================
 * Bounded buffer
 * ========================================================================= */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return 1;   /* signals caller to stop */
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* =========================================================================
 * Logging consumer thread
 * ========================================================================= */

static void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;

    while (1) {
        /* FIX 2: was bb_pop() — correct name is bounded_buffer_pop() */
        int rc = bounded_buffer_pop(buf, &item);
        if (rc == 1) break;   /* shutdown signal */
        if (rc != 0) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log",
                 LOG_DIR, item.container_id);
        mkdir(LOG_DIR, 0755);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) { perror("logging_thread: open"); continue; }

        size_t written = 0;
        while (written < item.length) {
            ssize_t n = write(fd, item.data + written,
                              item.length - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
        close(fd);
    }
    return NULL;
}

/* =========================================================================
 * Producer thread  (one per container; reads from pipe, pushes to buffer)
 * ========================================================================= */

static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(pa->pipe_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->container_id,
                CONTAINER_ID_LEN - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        /* FIX 2: was bb_push() — correct name is bounded_buffer_push() */
        bounded_buffer_push(pa->buf, &item);
    }
    close(pa->pipe_fd);
    free(pa);
    return NULL;
}

/* =========================================================================
 * Container child function  (runs inside clone()'d process)
 * ========================================================================= */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    dup2(cfg->pipe_fd, STDOUT_FILENO);
    dup2(cfg->pipe_fd, STDERR_FILENO);
    close(cfg->pipe_fd);

    if (cfg->nice_value != 0)
        setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    if (chroot(cfg->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/")           < 0) { perror("chdir");  return 1; }

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc",
          MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    char *const argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv);
    perror("execv");
    return 127;
}

/* =========================================================================
 * Kernel monitor helpers
 * ========================================================================= */

static int register_with_monitor(int monitor_fd,
                                  const char *container_id,
                                  pid_t host_pid,
                                  unsigned long soft_limit_bytes,
                                  unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id,
            sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

/* FIX 5: was missing 'static' — made static for consistency */
static int unregister_from_monitor(int monitor_fd,
                                    const char *container_id,
                                    pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id,
            sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* =========================================================================
 * Container metadata helpers
 * ========================================================================= */

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                           const char *id)
{
    container_record_t *r = ctx->containers;
    while (r) {
        if (strncmp(r->id, id, CONTAINER_ID_LEN) == 0)
            return r;
        r = r->next;
    }
    return NULL;
}

/* =========================================================================
 * Signal handlers
 * ========================================================================= */

static void sigchld_handler(int sig)
{
    (void)sig;
    if (!global_ctx) return;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&global_ctx->metadata_lock);
        container_record_t *r = global_ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                if (WIFEXITED(status)) {
                    r->exit_code   = WEXITSTATUS(status);
                    r->exit_signal = 0;
                    r->state = r->stop_requested
                               ? CONTAINER_STOPPED : CONTAINER_EXITED;
                    r->term_reason = r->stop_requested
                               ? TERM_STOPPED : TERM_EXITED;
                } else if (WIFSIGNALED(status)) {
                    r->exit_signal = WTERMSIG(status);
                    r->exit_code   = 128 + r->exit_signal;
                    if (r->stop_requested) {
                        r->state       = CONTAINER_STOPPED;
                        r->term_reason = TERM_STOPPED;
                    } else if (r->exit_signal == SIGKILL) {
                        r->state       = CONTAINER_KILLED;
                        r->term_reason = TERM_HARD_LIMIT;
                    } else {
                        r->state       = CONTAINER_EXITED;
                        r->term_reason = TERM_EXITED;
                    }
                }
                if (global_ctx->monitor_fd >= 0)
                    unregister_from_monitor(global_ctx->monitor_fd,
                                            r->id, r->host_pid);
                break;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&global_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (global_ctx) global_ctx->should_stop = 1;
}

/* =========================================================================
 * Launch a container
 * ========================================================================= */

static container_record_t *launch_container(supervisor_ctx_t *ctx,
                                             const control_request_t *req)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return NULL; }

    child_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,       PATH_MAX - 1);
    strncpy(cfg->command, req->command,      CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->pipe_fd    = pipefd[1];

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg); close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, cfg);
    int saved_errno = errno;
    close(pipefd[1]);
    free(stack);
    free(cfg);

    if (pid < 0) {
        errno = saved_errno;
        perror("clone");
        close(pipefd[0]);
        return NULL;
    }

    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) { close(pipefd[0]); return NULL; }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log",
             LOG_DIR, req->container_id);

    producer_arg_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->pipe_fd = pipefd[0];
        strncpy(pa->container_id, req->container_id,
                CONTAINER_ID_LEN - 1);
        pa->buf = &ctx->log_buffer;
        pthread_t pt;
        if (pthread_create(&pt, NULL, producer_thread, pa) != 0) {
            free(pa);
            close(pipefd[0]);
        } else {
            pthread_detach(pt);
        }
    } else {
        close(pipefd[0]);
    }

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, rec->id, pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes);
    return rec;
}

/* =========================================================================
 * Handle one request from a CLI client
 * FIX 3: The stray '/' character that appeared before this function
 *        definition has been removed.
 * ========================================================================= */

static void handle_request(supervisor_ctx_t *ctx, int cfd)
{
    control_request_t  req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (recv(cfd, &req, sizeof(req), MSG_WAITALL)
            != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "bad request");
        send(cfd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START:
    case CMD_RUN: {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (find_container(ctx, req.container_id)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' already exists", req.container_id);
            send(cfd, &resp, sizeof(resp), 0);
            return;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        container_record_t *rec = launch_container(ctx, &req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to launch '%s'", req.container_id);
            send(cfd, &resp, sizeof(resp), 0);
            return;
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        rec->next       = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (req.kind == CMD_START) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "started '%s' pid=%d",
                     req.container_id, rec->host_pid);
            send(cfd, &resp, sizeof(resp), 0);
        } else {
            /* CMD_RUN: send initial ack, then block until container exits */
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "running '%s' pid=%d (waiting...)",
                     req.container_id, rec->host_pid);
            send(cfd, &resp, sizeof(resp), 0);

            int ws;
            pid_t wpid = waitpid(rec->host_pid, &ws, 0);
            if (wpid > 0) {
                pthread_mutex_lock(&ctx->metadata_lock);
                if (WIFEXITED(ws)) {
                    rec->exit_code   = WEXITSTATUS(ws);
                    rec->exit_signal = 0;
                    rec->state = rec->stop_requested
                                 ? CONTAINER_STOPPED : CONTAINER_EXITED;
                    rec->term_reason = rec->stop_requested
                                 ? TERM_STOPPED : TERM_EXITED;
                } else if (WIFSIGNALED(ws)) {
                    rec->exit_signal = WTERMSIG(ws);
                    rec->exit_code   = 128 + rec->exit_signal;
                    if (rec->stop_requested) {
                        rec->state       = CONTAINER_STOPPED;
                        rec->term_reason = TERM_STOPPED;
                    } else if (rec->exit_signal == SIGKILL) {
                        rec->state       = CONTAINER_KILLED;
                        rec->term_reason = TERM_HARD_LIMIT;
                    } else {
                        rec->state       = CONTAINER_EXITED;
                        rec->term_reason = TERM_EXITED;
                    }
                }
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd,
                                            rec->id, rec->host_pid);
                pthread_mutex_unlock(&ctx->metadata_lock);
            }
            memset(&resp, 0, sizeof(resp));
            resp.status    = rec->exit_code;
            resp.exit_code = rec->exit_code;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' exited code=%d signal=%d reason=%s",
                     req.container_id, rec->exit_code,
                     rec->exit_signal,
                     term_reason_string(rec->term_reason));
            send(cfd, &resp, sizeof(resp), 0);
        }
        break;
    }

    case CMD_PS: {
        char buf[4096];
        int  off = 0;
        /* FIX 6: cast off to size_t for the subtraction passed to snprintf */
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "%-16s %-8s %-10s %-10s %-10s %-16s\n",
                        "ID", "PID", "STATE",
                        "SOFT(MiB)", "HARD(MiB)", "REASON");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = ctx->containers;
        while (r && off < (int)sizeof(buf) - 1) {
            off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                            "%-16s %-8d %-10s %-10lu %-10lu %-16s\n",
                            r->id, r->host_pid,
                            state_to_string(r->state),
                            r->soft_limit_bytes >> 20,
                            r->hard_limit_bytes >> 20,
                            term_reason_string(r->term_reason));
            r = r->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        strncpy(resp.message, buf, sizeof(resp.message) - 1);
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_LOGS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = find_container(ctx, req.container_id);
        char lp[PATH_MAX] = {0};
        if (rec) strncpy(lp, rec->log_path, PATH_MAX - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "unknown container '%s'", req.container_id);
            send(cfd, &resp, sizeof(resp), 0);
            return;
        }
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "LOG:%s", lp);
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = find_container(ctx, req.container_id);
        if (!rec || rec->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "'%s' not running", req.container_id);
            send(cfd, &resp, sizeof(resp), 0);
            return;
        }
        rec->stop_requested = 1;   /* set BEFORE signal for attribution */
        pid_t pid = rec->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(pid, SIGTERM);

        struct timespec ts;
        memset(&ts, 0, sizeof(ts));
        ts.tv_nsec = 100000000L;   /* 100 ms per iteration */
        int done = 0;
        for (int i = 0; i < 30 && !done; i++) {
            nanosleep(&ts, NULL);
            if (waitpid(pid, NULL, WNOHANG) == pid) done = 1;
        }
        if (!done) kill(pid, SIGKILL);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "stopped '%s'", req.container_id);
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "unknown command");
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }
}

/* =========================================================================
 * Supervisor main loop
 * ========================================================================= */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    global_ctx = &ctx;

    mkdir(LOG_DIR, 0755);

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc) { perror("pthread_mutex_init"); return 1; }

    /* FIX 2: was bb_init() — correct name is bounded_buffer_init() */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc) { perror("bounded_buffer_init"); return 1; }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] Warning: no /dev/container_monitor — "
                "memory enforcement disabled\n");

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        { perror("bind"); return 1; }
    if (listen(ctx.server_fd, 8) < 0)
        { perror("listen"); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rc = pthread_create(&ctx.logger_thread, NULL,
                        logging_thread, &ctx.log_buffer);
    if (rc) { perror("pthread_create"); return 1; }

    fprintf(stderr, "[supervisor] Started. rootfs=%s socket=%s\n",
            rootfs, CONTROL_PATH);

    /* Make the accept socket non-blocking so we can poll should_stop */
    int fl = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, fl | O_NONBLOCK);

    while (!ctx.should_stop) {
        int cfd = accept(ctx.server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts;
                memset(&ts, 0, sizeof(ts));
                ts.tv_nsec = 50000000L;   /* 50 ms */
                nanosleep(&ts, NULL);
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_request(&ctx, cfd);
        close(cfd);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Send SIGTERM to all still-running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give containers a moment to exit gracefully */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    struct timespec ts2;
    memset(&ts2, 0, sizeof(ts2));
    ts2.tv_sec = 1;
    nanosleep(&ts2, NULL);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* FIX 2: was bb_shutdown / bb_destroy */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records and unregister from monitor */
    pthread_mutex_lock(&ctx.metadata_lock);
    r = ctx.containers;
    while (r) {
        container_record_t *nx = r->next;
        if (ctx.monitor_fd >= 0)
            unregister_from_monitor(ctx.monitor_fd, r->id, r->host_pid);
        free(r);
        r = nx;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    fprintf(stderr, "[supervisor] Clean exit.\n");
    return 0;
}

/* =========================================================================
 * CLI client — sends a request to the running supervisor
 * ========================================================================= */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    control_response_t resp;
    ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Incomplete response\n");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_LOGS && resp.status == 0 &&
        strncmp(resp.message, "LOG:", 4) == 0) {
        const char *path = resp.message + 4;
        FILE *f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "Cannot open log: %s\n", path);
            close(fd);
            return 1;
        }
        char line[512];
        while (fgets(line, sizeof(line), f)) fputs(line, stdout);
        fclose(f);
    } else if (req->kind == CMD_RUN && resp.status == 0) {
        /* Print the initial "running..." ack */
        printf("%s\n", resp.message);
        /* Wait for the final exit-status response */
        n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n == (ssize_t)sizeof(resp))
            printf("%s\n", resp.message);
    } else {
        printf("%s\n", resp.message);
    }

    close(fd);

    /*
     * FIX 4: The original code had:
     *   return
     *   if(resp.status == 0) return 0; else return 1;
     * which is a syntax error (bare 'return' with no value, followed by
     * dead code).  Replaced with a single, correct conditional return.
     */
    return (resp.status == 0) ? 0 : 1;
}

/* =========================================================================
 * CLI command entry points
 * ========================================================================= */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
