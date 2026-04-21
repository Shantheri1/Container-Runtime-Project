/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

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

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    int stop_requested;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
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
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
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
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    pid_t last_pid;
    char last_container[64];
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
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

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

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

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    
    mkdir("logs", 0755);
char fullpath[PATH_MAX];
snprintf(fullpath, sizeof(fullpath), "/home/shantheri/container_project/OS-Jackfruit/boilerplate/logs");
mkdir(fullpath, 0755);
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char filename[PATH_MAX];
        snprintf(filename, sizeof(filename), "%s/%s.log",
                 LOG_DIR, item.container_id);

      FILE *f = fopen(filename, "a");
if (!f) {
    perror("fopen failed");
} else {
    fprintf(f, "%.*s\n", (int)item.length, item.data);
    fclose(f);
    printf("LOG THREAD: Wrote to %s\n", filename);
    fflush(stdout);
}
    }
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */

int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // 1. Set hostname so the prompt looks like [container-id]
    sethostname(config->id, strlen(config->id));

    // 2. Isolate the mount namespace (remount private so host isn't affected)
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    // 3. Enter the Rootfs
    if (chroot(config->rootfs) != 0) {
        perror("child: chroot failed");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("child: chdir failed");
        return 1;
    }

    // 4. Mount /proc (Required for many tools like 'ps' or 'top')
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("child: mount /proc failed");
    }

    // 5. Redirect output to the log file descriptor if provided
    if (config->log_write_fd > 0) {
        dup2(config->log_write_fd, STDOUT_FILENO);
        dup2(config->log_write_fd, STDERR_FILENO);
        close(config->log_write_fd);
    }

    // 6. Execute the command provided by the user
    // We use /bin/sh -c "command" to allow for arguments
    char *args[] = {"/bin/sh", "-c", config->command, NULL};
    execv("/bin/sh", args);

    // If we reach here, exec failed
    perror("child: execv failed");
    return 1;
}


int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
 typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_args_t;
void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    while ((n = read(args->read_fd, item.data, sizeof(item.data) - 1)) > 0) {
        item.data[n] = '\0';
        item.length = n;
        strncpy(item.container_id, args->container_id,
                CONTAINER_ID_LEN - 1);
        bounded_buffer_push(args->log_buffer, &item);
    }

    close(args->read_fd);
    free(args);
    return NULL;
}
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */
    

ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
if (ctx.server_fd < 0) {
    perror("socket");
    return 1;
}

struct sockaddr_un addr;
memset(&addr, 0, sizeof(addr));
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

unlink(CONTROL_PATH);  // remove old socket

if (bind(ctx.server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
}

if (listen(ctx.server_fd, 5) < 0) {
    perror("listen");
    return 1;
}
ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
if (ctx.monitor_fd < 0) {
    perror("open /dev/container_monitor");
    printf("Warning: running without kernel monitor\n");
} else {
    printf("Kernel monitor opened successfully\n");
}
printf("Supervisor running...\n");
signal(SIGCHLD, SIG_DFL);   // child cleanup default
//signal(SIGINT, SIG_IGN);    // ignore Ctrl+C
signal(SIGTERM, SIG_IGN);   // ignore kill
pthread_t logger_thread;

if (pthread_create(&logger_thread, NULL, logging_thread, &ctx) != 0) {
    perror("pthread_create");
    return 1;
}

while (1) {
int status;
pid_t p;

while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
    container_record_t *curr = ctx.containers;

    while (curr) {
        if (curr->host_pid == p) {
            curr->state = CONTAINER_EXITED;
            printf("Container %s exited\n", curr->id);
            break;
        }
        curr = curr->next;
    }
}
    int client_fd = accept(ctx.server_fd, NULL, NULL);
if (client_fd < 0) {
    if (errno == EINTR) continue;   // signal interrupted us, just retry
    perror("accept");
    break;                          // real error, exit the loop
}
    control_request_t req;
    memset(&req, 0, sizeof(req));

   if (read(client_fd, &req, sizeof(req)) <= 0) {
   close(client_fd);
   continue;
   }
    printf("Received command: %d for container %s\n",
           req.kind, req.container_id);

    if (req.kind == CMD_START) {

        child_config_t *config = malloc(sizeof(child_config_t));
if (!config) {
    perror("malloc");
    continue;
}
memset(config, 0, sizeof(child_config_t));
int pipefds[2];
if (pipe(pipefds) < 0) {
    perror("pipe");
    free(config);
    close(client_fd);
    continue;
}
config->log_write_fd = pipefds[1];
        strcpy(config->id, req.container_id);
        strcpy(config->rootfs, req.rootfs);
        strcpy(config->command, req.command);

        char *stack = malloc(STACK_SIZE);
        if (!stack) {
            perror("malloc stack");
            continue;
        }
        memset(stack, 0, STACK_SIZE);
        printf("About to clone...\n");
        pid_t pid = clone(child_fn,
      stack + STACK_SIZE,
      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
      config);
printf("Clone returned PID:%d\n",pid);
close(pipefds[1]);  // parent closes write end
pipe_reader_args_t *pr_args = malloc(sizeof(pipe_reader_args_t));
pr_args->read_fd = pipefds[0];
strncpy(pr_args->container_id, req.container_id, CONTAINER_ID_LEN - 1);
pr_args->log_buffer = &ctx.log_buffer;

pthread_t reader_thread;
pthread_create(&reader_thread, NULL, pipe_reader_thread, pr_args);
pthread_detach(reader_thread);
        if (pid < 0) {
            perror("clone");
        } else {
            // after the clone() succeeds or fails, tell the client:
char buf[256];
if (pid < 0)
    snprintf(buf, sizeof(buf), "Failed to start container %s\n", req.container_id);
else
    snprintf(buf, sizeof(buf), "Started container %s with PID %d\n", req.container_id, pid);
    log_item_t item;
memset(&item, 0, sizeof(item));

strcpy(item.container_id, req.container_id);

snprintf(item.data, sizeof(item.data),
         "Container %s started with PID %d",
         req.container_id, pid);
item.length = strlen(item.data);
bounded_buffer_push(&ctx.log_buffer, &item);
write(client_fd, buf, strlen(buf));       
        container_record_t *newc= malloc(sizeof(container_record_t));
memset(newc, 0, sizeof(container_record_t));
strcpy(newc->id, req.container_id);
newc->host_pid = pid;
newc->state = CONTAINER_RUNNING;
newc->started_at = time(NULL);
snprintf(newc->log_path, sizeof(newc->log_path), "%s/%s.log",
         LOG_DIR, req.container_id);
newc->next = ctx.containers;
ctx.containers = newc;
if (ctx.monitor_fd >= 0) {
    register_with_monitor(ctx.monitor_fd,
                          req.container_id,
                          pid,
                          req.soft_limit_bytes,
                          req.hard_limit_bytes);
    printf("Registered container %s with monitor\n", req.container_id);
}
}
    }
    if (req.kind == CMD_PS) {
    char buf[4096];
    int offset = 0;
    container_record_t *curr = ctx.containers;
    if (!curr) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "No containers running\n");
    }
    while (curr) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                   "ID: %s | PID: %d | STATE: %s | STARTED: %ld\n",
                   curr->id,
                   curr->host_pid,
                   state_to_string(curr->state),
                   curr->started_at);
        curr = curr->next;
    }

    write(client_fd, buf, offset);
}
if (req.kind == CMD_STOP) {
    int found = 0;
    container_record_t *curr = ctx.containers;

    while (curr) {
        if (strcmp(curr->id, req.container_id) == 0) {
            curr->stop_requested = 1;
            kill(curr->host_pid, SIGKILL);
            if (ctx.monitor_fd >= 0) {
    unregister_from_monitor(ctx.monitor_fd,
                            curr->id,
                            curr->host_pid);
    printf("Unregistered container %s from monitor\n", curr->id);
}
            curr->state = CONTAINER_KILLED;
            printf("Container %s stopped\n", curr->id);
            found = 1;
            break;
        }
        curr = curr->next;
    }
    char buf[256];
    if (found)
        snprintf(buf, sizeof(buf), "Stopped container %s\n", req.container_id);
    else
        snprintf(buf, sizeof(buf), "Container %s not found\n", req.container_id);

    write(client_fd, buf, strlen(buf));
}
if (req.kind == CMD_LOGS) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char msg[] = "No logs found\n";
        write(client_fd, msg, strlen(msg));
    } else {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(client_fd, buf, n);
        close(fd);
    }
}
    close(client_fd);
    }
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
pthread_join(logger_thread, NULL);
bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 1;
}
/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sockfd;
    struct sockaddr_un addr;
    char response[4096];
    ssize_t n;

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    if (write(sockfd, req, sizeof(*req)) < 0) {
        perror("write");
        close(sockfd);
        return 1;
    }

    while ((n = read(sockfd, response, sizeof(response) - 1)) > 0) {
        response[n] = '\0';
        printf("%s", response);
    }

    close(sockfd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
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
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
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

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
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

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
