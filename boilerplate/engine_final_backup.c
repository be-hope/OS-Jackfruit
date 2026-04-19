#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <time.h>
#include <limits.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"

#define MAX_CONTAINERS 64
#define ID_LEN 32
#define CMD_LEN 256
#define ROOTFS_LEN 256
#define BUFFER_CAPACITY 32
#define CHUNK_SIZE 4096

#define DEFAULT_SOFT (40UL << 20)
#define DEFAULT_HARD (64UL << 20)

typedef enum {
    ST_STARTING = 0,
    ST_RUNNING,
    ST_STOPPED,
    ST_EXITED,
    ST_KILLED
} state_t;

typedef struct {
    char id[ID_LEN];
    pid_t pid;
    time_t started;
    state_t state;
    int exit_code;
    int exit_signal;
    unsigned long soft_limit;
    unsigned long hard_limit;
    char rootfs[ROOTFS_LEN];
    char logfile[PATH_MAX];
} container_t;

typedef struct {
    char id[ID_LEN];
    size_t len;
    char data[CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[BUFFER_CAPACITY];
    int head, tail, count;
    int shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} buffer_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    pthread_t logger_thread;
    buffer_t logbuf;
    pthread_mutex_t lock;
    container_t containers[MAX_CONTAINERS];
    int used;
} supervisor_t;

typedef struct {
    char id[ID_LEN];
    char rootfs[ROOTFS_LEN];
    char command[CMD_LEN];
    int write_fd;
} child_cfg_t;

static supervisor_t g_ctx;

/* ---------------- STATE STRING ---------------- */

const char *state_name(state_t s)
{
    switch (s) {
        case ST_STARTING: return "starting";
        case ST_RUNNING: return "running";
        case ST_STOPPED: return "stopped";
        case ST_EXITED: return "exited";
        case ST_KILLED: return "killed";
        default: return "unknown";
    }
}

/* ---------------- BUFFER ---------------- */

void buffer_init(buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

void buffer_shutdown(buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutdown = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

int buffer_push(buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == BUFFER_CAPACITY && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutdown) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int buffer_pop(buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutdown) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ---------------- LOGGER ---------------- */

void *logger_thread(void *arg)
{
    supervisor_t *ctx = (supervisor_t *)arg;
    log_item_t item;

    while (buffer_pop(&ctx->logbuf, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.id);

        FILE *fp = fopen(path, "a");
        if (fp) {
            fwrite(item.data, 1, item.len, fp);
            fclose(fp);
        }
    }

    return NULL;
}

/* ---------------- MONITOR ---------------- */

void monitor_register_pid(int fd, const char *id, pid_t pid,
                          unsigned long soft, unsigned long hard)
{
    if (fd < 0) return;

    struct monitor_request req;
    memset(&req, 0, sizeof(req));

    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id)-1);

    ioctl(fd, MONITOR_REGISTER, &req);
}

void monitor_unregister_pid(int fd, const char *id, pid_t pid)
{
    if (fd < 0) return;

    struct monitor_request req;
    memset(&req, 0, sizeof(req));

    req.pid = pid;
    strncpy(req.container_id, id, sizeof(req.container_id)-1);

    ioctl(fd, MONITOR_UNREGISTER, &req);
}

/* ---------------- CHILD PROCESS ---------------- */

int child_fn(void *arg)
{
    child_cfg_t *cfg = (child_cfg_t *)arg;

    sethostname(cfg->id, strlen(cfg->id));

    chroot(cfg->rootfs);
    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->write_fd, STDOUT_FILENO);
    dup2(cfg->write_fd, STDERR_FILENO);
    close(cfg->write_fd);

    execlp(cfg->command, cfg->command, NULL);

    perror("exec failed");
    return 1;
}
/* ---------------- HELPERS ---------------- */

int find_container(const char *id)
{
    for (int i = 0; i < g_ctx.used; i++) {
        if (strcmp(g_ctx.containers[i].id, id) == 0)
            return i;
    }
    return -1;
}

void reap_children()
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx.lock);

        for (int i = 0; i < g_ctx.used; i++) {
            if (g_ctx.containers[i].pid == pid) {

                if (WIFEXITED(status)) {
                    g_ctx.containers[i].state = ST_EXITED;
                    g_ctx.containers[i].exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    g_ctx.containers[i].state = ST_KILLED;
                    g_ctx.containers[i].exit_signal = WTERMSIG(status);
                }

                monitor_unregister_pid(
                    g_ctx.monitor_fd,
                    g_ctx.containers[i].id,
                    pid
                );
            }
        }

        pthread_mutex_unlock(&g_ctx.lock);
    }
}

void sigchld_handler(int sig)
{
    (void)sig;
    reap_children();
}

/* ---------------- START CONTAINER ---------------- */

void start_container(const char *id,
                     const char *rootfs,
                     const char *cmd,
                     int wait_mode)
{
    int pipefd[2];
    pipe(pipefd);

    child_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.id, id, sizeof(cfg.id)-1);
    strncpy(cfg.rootfs, rootfs, sizeof(cfg.rootfs)-1);
    strncpy(cfg.command, cmd, sizeof(cfg.command)-1);
    cfg.write_fd = pipefd[1];

    void *stack = malloc(STACK_SIZE);

    pid_t pid = clone(
        child_fn,
        (char *)stack + STACK_SIZE,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        &cfg
    );

    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        return;
    }

    pthread_mutex_lock(&g_ctx.lock);

    container_t *c = &g_ctx.containers[g_ctx.used++];

    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id)-1);
    strncpy(c->rootfs, rootfs, sizeof(c->rootfs)-1);

    c->pid = pid;
    c->started = time(NULL);
    c->state = ST_RUNNING;
    c->soft_limit = DEFAULT_SOFT;
    c->hard_limit = DEFAULT_HARD;

    snprintf(c->logfile, sizeof(c->logfile),
             "%s/%s.log", LOG_DIR, id);

    pthread_mutex_unlock(&g_ctx.lock);

    monitor_register_pid(
        g_ctx.monitor_fd,
        id,
        pid,
        DEFAULT_SOFT,
        DEFAULT_HARD
    );

    /* read child output -> bounded buffer */
    char buf[CHUNK_SIZE];
    int n;

    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));

        strncpy(item.id, id, sizeof(item.id)-1);
        memcpy(item.data, buf, n);
        item.len = n;

        buffer_push(&g_ctx.logbuf, &item);
    }

    close(pipefd[0]);

    if (wait_mode)
        waitpid(pid, NULL, 0);

    printf("Started container %s PID=%d\n", id, pid);
}

/* ---------------- COMMAND HANDLERS ---------------- */

void cmd_ps()
{
    pthread_mutex_lock(&g_ctx.lock);

    printf("ID\tPID\tSTATE\n");

    for (int i = 0; i < g_ctx.used; i++) {
        printf("%s\t%d\t%s\n",
               g_ctx.containers[i].id,
               g_ctx.containers[i].pid,
               state_name(g_ctx.containers[i].state));
    }

    pthread_mutex_unlock(&g_ctx.lock);
}

void cmd_logs(const char *id)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, id);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("No logs found.\n");
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp))
        printf("%s", line);

    fclose(fp);
}

void cmd_stop(const char *id)
{
    pthread_mutex_lock(&g_ctx.lock);

    int idx = find_container(id);

    if (idx >= 0) {
        kill(g_ctx.containers[idx].pid, SIGTERM);
        g_ctx.containers[idx].state = ST_STOPPED;
        printf("Stopped %s\n", id);
    } else {
        printf("Container not found\n");
    }

    pthread_mutex_unlock(&g_ctx.lock);
}

/* ---------------- SUPERVISOR ---------------- */

int run_supervisor(const char *base_rootfs)
{
    (void)base_rootfs;

    memset(&g_ctx, 0, sizeof(g_ctx));

    mkdir(LOG_DIR, 0777);

    pthread_mutex_init(&g_ctx.lock, NULL);
    buffer_init(&g_ctx.logbuf);

    pthread_create(
        &g_ctx.logger_thread,
        NULL,
        logger_thread,
        &g_ctx
    );

    signal(SIGCHLD, sigchld_handler);

    g_ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    unlink(SOCKET_PATH);

    g_ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    bind(g_ctx.server_fd,
         (struct sockaddr *)&addr,
         sizeof(addr));

    listen(g_ctx.server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        int client = accept(g_ctx.server_fd, NULL, NULL);

        if (client < 0)
            continue;

        char req[512];
        memset(req, 0, sizeof(req));

        read(client, req, sizeof(req)-1);

        char cmd[32], id[64], rootfs[256], prog[256];

        sscanf(req, "%s", cmd);

        if (strcmp(cmd, "start") == 0) {
            sscanf(req, "%*s %s %s %s", id, rootfs, prog);
            start_container(id, rootfs, prog, 0);
        }
        else if (strcmp(cmd, "run") == 0) {
            sscanf(req, "%*s %s %s %s", id, rootfs, prog);
            start_container(id, rootfs, prog, 1);
        }
        else if (strcmp(cmd, "ps") == 0) {
            cmd_ps();
        }
        else if (strcmp(cmd, "logs") == 0) {
            sscanf(req, "%*s %s", id);
            cmd_logs(id);
        }
        else if (strcmp(cmd, "stop") == 0) {
            sscanf(req, "%*s %s", id);
            cmd_stop(id);
        }

        close(client);
    }

    return 0;
}

/* ---------------- CLIENT ---------------- */

void send_request(const char *msg)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return;
    }

    write(fd, msg, strlen(msg));
    close(fd);
}

/* ---------------- MAIN ---------------- */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("./engine supervisor <base-rootfs>\n");
        printf("./engine start <id> <rootfs> <cmd>\n");
        printf("./engine run <id> <rootfs> <cmd>\n");
        printf("./engine ps\n");
        printf("./engine logs <id>\n");
        printf("./engine stop <id>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) return 1;
        return run_supervisor(argv[2]);
    }

    char msg[512] = {0};

    if (strcmp(argv[1], "start") == 0 && argc >= 5)
        snprintf(msg, sizeof(msg),
                 "start %s %s %s",
                 argv[2], argv[3], argv[4]);

    else if (strcmp(argv[1], "run") == 0 && argc >= 5)
        snprintf(msg, sizeof(msg),
                 "run %s %s %s",
                 argv[2], argv[3], argv[4]);

    else if (strcmp(argv[1], "ps") == 0)
        strcpy(msg, "ps");

    else if (strcmp(argv[1], "logs") == 0 && argc >= 3)
        snprintf(msg, sizeof(msg),
                 "logs %s", argv[2]);

    else if (strcmp(argv[1], "stop") == 0 && argc >= 3)
        snprintf(msg, sizeof(msg),
                 "stop %s", argv[2]);

    else {
        printf("Invalid command\n");
        return 1;
    }

    send_request(msg);
    return 0;
}
