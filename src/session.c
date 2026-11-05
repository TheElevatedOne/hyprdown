#include "hyprdown.h"

#include <errno.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POLL_INTERVAL_MS 100
#define RECLOSE_INTERVAL 1.0
#define KILL_GRACE 1.0
/* Time an app gets to finish exiting by itself after its last window closed. */
#define TERM_GRACE 1.0
#define MAX_CHILDREN 512

/* Never signalled, even if they own a window or are compositor children. */
static const char *const IGNORED_COMMS[] = {
    "Hyprland",
    "Xwayland",
    "hyprdown",
    "start-hyprland",
    "uwsm",
    NULL,
};

static char *xstrdup(const char *s) {
    char *d = strdup(s ? s : "");
    if (!d) {
        fprintf(stderr, "hyprdown: out of memory\n");
        exit(1);
    }
    return d;
}

static void *xgrow(void *ptr, size_t *cap, size_t count, size_t size) {
    if (count < *cap)
        return ptr;
    size_t ncap = *cap ? *cap * 2 : 32;
    void *n = realloc(ptr, ncap * size);
    if (!n) {
        fprintf(stderr, "hyprdown: out of memory\n");
        exit(1);
    }
    *cap = ncap;
    return n;
}

static double monotonic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static const char *json_str(struct json_object *obj, const char *key) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val) ||
        !json_object_is_type(val, json_type_string))
        return NULL;
    return json_object_get_string(val);
}

static pid_t json_pid(struct json_object *obj) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, "pid", &val) ||
        !json_object_is_type(val, json_type_int))
        return -1;
    int64_t pid = json_object_get_int64(val);
    return pid > 0 && pid <= INT32_MAX ? (pid_t)pid : -1;
}

static struct json_object *fetch_json(const char *cmd, json_type type) {
    char *err = NULL;
    char *reply = hypr_request(cmd, &err);

    if (!reply) {
        free(err);
        return NULL;
    }

    struct json_object *root = json_tokener_parse(reply);
    free(reply);
    if (root && !json_object_is_type(root, type)) {
        json_object_put(root);
        root = NULL;
    }
    return root;
}

static int ignored_comm(const char *comm) {
    for (size_t i = 0; IGNORED_COMMS[i]; i++) {
        if (strcmp(comm, IGNORED_COMMS[i]) == 0)
            return 1;
    }
    return 0;
}

static int pid_protected(const struct Session *s, pid_t pid) {
    if (pid <= 1 || pid == getpid() || pid == s->compositor_pid)
        return 1;
    for (size_t i = 0; i < s->nprotected; i++) {
        if (s->protected_pids[i] == pid)
            return 1;
    }
    return 0;
}

static struct Proc *find_proc(const struct Session *s, pid_t pid) {
    for (size_t i = 0; i < s->nprocs; i++) {
        if (s->procs[i].pid == pid)
            return &s->procs[i];
    }
    return NULL;
}

/* Track pid as a process that must exit. Skips protected pids, other users'
 * processes, zombies and the compositor's own helpers. */
static void add_proc(struct Session *s, pid_t pid) {
    struct ProcInfo info;

    if (pid <= 0 || pid_protected(s, pid) || find_proc(s, pid))
        return;
    if (proc_info(pid, &info) < 0)
        return;
    if (info.uid != getuid() || info.state == 'Z' || info.state == 'X')
        return;
    if (ignored_comm(info.comm))
        return;

    s->procs = xgrow(s->procs, &s->capprocs, s->nprocs, sizeof(*s->procs));
    struct Proc *p = &s->procs[s->nprocs++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->starttime = info.starttime;
    p->name = xstrdup(info.comm);
    p->alive = 1;
}

static int proc_still_alive(const struct Proc *p) {
    struct ProcInfo info;

    if (proc_info(p->pid, &info) < 0)
        return 0;
    if (info.state == 'Z' || info.state == 'X')
        return 0;
    return info.starttime == p->starttime;
}

static void proc_signal(struct Proc *p, int sig) {
    /* Re-check identity right before signalling to avoid hitting a reused pid. */
    if (!proc_still_alive(p)) {
        p->alive = 0;
        return;
    }
    if (kill(p->pid, sig) != 0 && errno != ESRCH)
        fprintf(stderr, "hyprdown: %s %s (pid %d): %s\n", sig == SIGKILL ? "SIGKILL" : "SIGTERM",
                p->name, (int)p->pid, strerror(errno));
}

static struct Window *find_window(const struct Session *s, const char *address) {
    for (size_t i = 0; i < s->nwindows; i++) {
        if (strcmp(s->windows[i].address, address) == 0)
            return &s->windows[i];
    }
    return NULL;
}

/* Sync the window list with Hyprland. Also picks up windows opened after the
 * first scan, such as an app restarting a helper while it is being closed. */
static void sync_windows(struct Session *s, struct json_object *clients) {
    size_t n = clients ? json_object_array_length(clients) : 0;

    for (size_t i = 0; i < s->nwindows; i++)
        s->windows[i].present = 0;

    for (size_t i = 0; i < n; i++) {
        struct json_object *el = json_object_array_get_idx(clients, i);
        if (!el || !json_object_is_type(el, json_type_object))
            continue;

        const char *address = json_str(el, "address");
        if (!address || address[0] == '\0')
            continue;

        struct Window *w = find_window(s, address);
        if (!w) {
            s->windows = xgrow(s->windows, &s->capwindows, s->nwindows, sizeof(*s->windows));
            w = &s->windows[s->nwindows++];
            memset(w, 0, sizeof(*w));
            w->address = xstrdup(address);
            w->class_name = xstrdup(json_str(el, "class"));
            w->title = xstrdup(json_str(el, "title"));
            w->pid = json_pid(el);
        }
        w->present = 1;

        add_proc(s, w->pid);
        struct Proc *p = find_proc(s, w->pid);
        if (p)
            p->had_window = 1;
    }
}

/* Layer-shell clients (bars, wallpapers, notification daemons) cannot be
 * closed through the compositor; their processes are terminated instead. */
static void collect_layers(struct Session *s) {
    struct json_object *root = fetch_json("j/layers", json_type_object);
    if (!root)
        return;

    json_object_object_foreach(root, monitor_name, monitor) {
        (void)monitor_name;
        struct json_object *levels = NULL;
        if (!json_object_is_type(monitor, json_type_object) ||
            !json_object_object_get_ex(monitor, "levels", &levels) ||
            !json_object_is_type(levels, json_type_object))
            continue;

        json_object_object_foreach(levels, level_name, arr) {
            (void)level_name;
            if (!json_object_is_type(arr, json_type_array))
                continue;
            size_t n = json_object_array_length(arr);
            for (size_t i = 0; i < n; i++) {
                struct json_object *el = json_object_array_get_idx(arr, i);
                if (el && json_object_is_type(el, json_type_object))
                    add_proc(s, json_pid(el));
            }
        }
    }

    json_object_put(root);
}

/* Daemons started by exec-once have no window but are compositor children. */
static void collect_children(struct Session *s) {
    pid_t pids[MAX_CHILDREN];

    if (s->compositor_pid <= 1)
        return;

    int n = proc_collect_children(s->compositor_pid, pids, MAX_CHILDREN);
    for (int i = 0; i < n; i++)
        add_proc(s, pids[i]);
}

int session_collect(struct Session *s) {
    struct json_object *clients = fetch_json("j/clients", json_type_array);
    if (!clients) {
        fprintf(stderr, "hyprdown: failed to list clients from Hyprland\n");
        return -1;
    }

    sync_windows(s, clients);
    json_object_put(clients);
    collect_layers(s);
    collect_children(s);
    return 0;
}

void session_free(struct Session *s) {
    for (size_t i = 0; i < s->nwindows; i++) {
        free(s->windows[i].address);
        free(s->windows[i].class_name);
        free(s->windows[i].title);
    }
    for (size_t i = 0; i < s->nprocs; i++)
        free(s->procs[i].name);
    free(s->windows);
    free(s->procs);
    s->windows = NULL;
    s->procs = NULL;
    s->nwindows = s->capwindows = 0;
    s->nprocs = s->capprocs = 0;
}

static int pid_in(struct json_object *clients, pid_t pid) {
    size_t n = clients ? json_object_array_length(clients) : 0;
    for (size_t i = 0; i < n; i++) {
        if (json_pid(json_object_array_get_idx(clients, i)) == pid)
            return 1;
    }
    return 0;
}

static void close_windows(struct Session *s) {
    for (size_t i = 0; i < s->nwindows; i++) {
        struct Window *w = &s->windows[i];
        if (!w->present)
            continue;

        char *failure = hypr_close_window(w->address, s->use_lua);
        if (failure && !w->warned) {
            fprintf(stderr, "hyprdown: close %s: %s\n", w->class_name, failure);
            w->warned = 1;
        }
        free(failure);
    }
}

/* Update window/process state. A process is sent SIGTERM once, and only when
 * it has no windows left, so apps get the chance to close gracefully through
 * their windows first. An app that had windows gets TERM_GRACE to exit by itself
 * before that. Returns how many windows and processes are still around. */
static size_t refresh(struct Session *s) {
    struct json_object *clients = fetch_json("j/clients", json_type_array);
    const double now = monotonic_now();
    size_t remaining = 0;

    /* If IPC fails the compositor is going away; its windows go with it. */
    sync_windows(s, clients);
    for (size_t i = 0; i < s->nwindows; i++)
        remaining += s->windows[i].present;

    for (size_t i = 0; i < s->nprocs; i++) {
        struct Proc *p = &s->procs[i];
        if (!p->alive)
            continue;
        p->alive = proc_still_alive(p);
        if (!p->alive)
            continue;
        remaining++;
        if (pid_in(clients, p->pid)) {
            p->windows_gone_at = 0;
            continue;
        }
        if (p->had_window && p->windows_gone_at == 0)
            p->windows_gone_at = now;
        if (!p->termed && (!p->had_window || now - p->windows_gone_at >= TERM_GRACE)) {
            proc_signal(p, SIGTERM);
            p->termed = 1;
        }
    }

    if (clients)
        json_object_put(clients);
    return remaining;
}

static void print_remaining(const struct Session *s, double left) {
    size_t shown = 0;
    size_t total = 0;

    for (size_t i = 0; i < s->nwindows; i++)
        total += s->windows[i].present;
    for (size_t i = 0; i < s->nprocs; i++)
        total += s->procs[i].alive;

    fprintf(stderr, "hyprdown: waiting for %zu item(s) (%.1fs left)\n", total, left);

    for (size_t i = 0; i < s->nwindows && shown < 8; i++) {
        const struct Window *w = &s->windows[i];
        if (!w->present)
            continue;
        fprintf(stderr, "  - window %s", w->class_name[0] ? w->class_name : "unknown");
        if (w->title[0])
            fprintf(stderr, " (%s)", w->title);
        fputc('\n', stderr);
        shown++;
    }
    for (size_t i = 0; i < s->nprocs && shown < 8; i++) {
        const struct Proc *p = &s->procs[i];
        if (!p->alive)
            continue;
        fprintf(stderr, "  - process %s (pid %d)\n", p->name, (int)p->pid);
        shown++;
    }
    if (total > shown)
        fprintf(stderr, "  - ...\n");
}

void session_print(const struct Session *s) {
    printf("Dispatchers: %s\n", s->use_lua ? "lua" : "legacy");
    if (s->compositor_pid > 0)
        printf("Compositor pid: %d\n", (int)s->compositor_pid);
    else
        printf("Compositor pid: unknown (compositor children are not collected)\n");

    printf("\nWindows to close (%zu):\n", s->nwindows);
    for (size_t i = 0; i < s->nwindows; i++) {
        const struct Window *w = &s->windows[i];
        printf("  %s  %s", w->address, w->class_name[0] ? w->class_name : "unknown");
        if (w->title[0])
            printf(" (%s)", w->title);
        if (w->pid > 0)
            printf("  pid %d", (int)w->pid);
        putchar('\n');
    }

    printf("\nProcesses to wait for (%zu):\n", s->nprocs);
    for (size_t i = 0; i < s->nprocs; i++) {
        const struct Proc *p = &s->procs[i];
        int has_window = 0;
        for (size_t j = 0; j < s->nwindows && !has_window; j++)
            has_window = s->windows[j].pid == p->pid;
        printf("  %-8d %-16s %s\n", (int)p->pid, p->name,
               has_window ? "SIGTERM after its windows close" : "SIGTERM");
    }
}

int session_close(struct Session *s, unsigned timeout_sec) {
    const double start = monotonic_now();
    const double deadline = start + (double)timeout_sec;
    double last_close = start;
    double last_print = start;
    size_t remaining;

    close_windows(s);
    remaining = refresh(s);

    while (remaining > 0) {
        double now = monotonic_now();
        if (now >= deadline)
            break;

        if (now - last_print >= 1.0) {
            print_remaining(s, deadline - now);
            last_print = now;
        }

        sleep_ms(POLL_INTERVAL_MS);

        /* Some apps ignore the first close (e.g. while still starting up). */
        if (monotonic_now() - last_close >= RECLOSE_INTERVAL) {
            close_windows(s);
            last_close = monotonic_now();
        }

        remaining = refresh(s);
    }

    if (remaining == 0) {
        fprintf(stderr, "hyprdown: all apps exited\n");
        return 0;
    }

    fprintf(stderr, "hyprdown: timeout after %us, killing what is left\n", timeout_sec);
    print_remaining(s, 0);

    for (size_t i = 0; i < s->nprocs; i++) {
        if (s->procs[i].alive)
            proc_signal(&s->procs[i], SIGKILL);
    }

    const double kill_deadline = monotonic_now() + KILL_GRACE;
    while (remaining > 0 && monotonic_now() < kill_deadline) {
        sleep_ms(POLL_INTERVAL_MS);
        remaining = refresh(s);
    }

    if (remaining > 0) {
        fprintf(stderr, "hyprdown: %zu item(s) could not be closed\n", remaining);
        print_remaining(s, 0);
    }
    return 1;
}
