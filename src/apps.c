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

static const char *const IGNORE_DAEMONS[] = {
    "Xwayland",
    "hyprdown",
    NULL,
};

static int ignored_name(const char *name) {
    if (!name)
        return 0;
    for (size_t i = 0; IGNORE_DAEMONS[i]; i++) {
        if (strcmp(name, IGNORE_DAEMONS[i]) == 0)
            return 1;
    }
    return 0;
}

static char *xstrdup(const char *s) {
    if (!s)
        return NULL;
    char *d = strdup(s);
    if (!d) {
        fprintf(stderr, "hyprdown: out of memory\n");
        exit(1);
    }
    return d;
}

static int json_get_string(struct json_object *obj, const char *key, const char **out) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val) ||
        !json_object_is_type(val, json_type_string))
        return 0;
    *out = json_object_get_string(val);
    return *out != NULL;
}

static int json_get_int64(struct json_object *obj, const char *key, int64_t *out) {
    struct json_object *val = NULL;
    if (!json_object_object_get_ex(obj, key, &val))
        return 0;
    if (!json_object_is_type(val, json_type_int) &&
        !json_object_is_type(val, json_type_double))
        return 0;
    *out = json_object_get_int64(val);
    return 1;
}

static int apps_reserve(struct AppList *list, size_t extra) {
    if (list->count + extra <= list->cap)
        return 0;
    size_t ncap = list->cap ? list->cap * 2 : 32;
    while (ncap < list->count + extra)
        ncap *= 2;
    struct App *nitems = realloc(list->items, ncap * sizeof(*nitems));
    if (!nitems)
        return -1;
    list->items = nitems;
    list->cap = ncap;
    return 0;
}

static int apps_has_address(const struct AppList *list, const char *address) {
    if (!address || address[0] == '\0')
        return 0;
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].address && strcmp(list->items[i].address, address) == 0)
            return 1;
    }
    return 0;
}

static int apps_has_pid_only(const struct AppList *list, pid_t pid) {
    if (pid <= 0)
        return 0;
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].pid == pid &&
            (!list->items[i].address || list->items[i].address[0] == '\0'))
            return 1;
    }
    return 0;
}

static int apps_add(struct AppList *list, const char *address, const char *class_name,
                    const char *title, pid_t pid, int is_layer, int always_signal) {
    if (address && address[0] != '\0' && apps_has_address(list, address))
        return 0;
    if ((!address || address[0] == '\0') && apps_has_pid_only(list, pid))
        return 0;
    if (pid > 0 && (pid == getpid() || pid == getppid()))
        return 0;
    if (ignored_name(class_name))
        return 0;

    if (apps_reserve(list, 1) < 0)
        return -1;

    struct App *app = &list->items[list->count];
    memset(app, 0, sizeof(*app));
    app->address = address && address[0] ? xstrdup(address) : NULL;
    app->class_name = class_name && class_name[0] ? xstrdup(class_name) : xstrdup("unknown");
    app->title = title ? xstrdup(title) : xstrdup("");
    app->pid = pid;
    app->is_layer = is_layer;
    app->always_signal = always_signal || is_layer;
    list->count++;
    return 0;
}

void apps_free(struct AppList *list) {
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].address);
        free(list->items[i].class_name);
        free(list->items[i].title);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int parse_app_object(struct json_object *obj, struct AppList *list, int force_layer) {
    const char *address = NULL;
    const char *class_name = NULL;
    const char *title = NULL;
    const char *ns = NULL;
    int64_t pid64 = -1;
    pid_t pid = -1;
    int is_layer = force_layer;

    json_get_string(obj, "address", &address);
    json_get_string(obj, "class", &class_name);
    json_get_string(obj, "title", &title);
    if (json_get_string(obj, "namespace", &ns)) {
        is_layer = 1;
        if (!class_name || class_name[0] == '\0')
            class_name = ns;
    }
    if (json_get_int64(obj, "pid", &pid64) && pid64 > 0)
        pid = (pid_t)pid64;

    if ((!address || address[0] == '\0') && pid <= 0)
        return 0;

    return apps_add(list, address, class_name, title, pid, is_layer, is_layer);
}

static int collect_clients(struct AppList *list) {
    char *err = NULL;
    char *reply = hypr_request("j/clients", &err);
    if (!reply) {
        fprintf(stderr, "hyprdown: failed to list clients: %s\n",
                err ? err : "unknown error");
        free(err);
        return -1;
    }
    free(err);

    struct json_object *root = json_tokener_parse(reply);
    free(reply);
    if (!root || !json_object_is_type(root, json_type_array)) {
        fprintf(stderr, "hyprdown: Hyprland returned invalid clients JSON\n");
        if (root)
            json_object_put(root);
        return -1;
    }

    int n = json_object_array_length(root);
    for (int i = 0; i < n; i++) {
        struct json_object *el = json_object_array_get_idx(root, i);
        if (!el || !json_object_is_type(el, json_type_object))
            continue;
        if (parse_app_object(el, list, 0) < 0) {
            json_object_put(root);
            return -1;
        }
    }

    json_object_put(root);
    return 0;
}

static int collect_layers(struct AppList *list) {
    char *err = NULL;
    char *reply = hypr_request("j/layers", &err);
    if (!reply) {
        /* layers are optional; some setups may not expose them */
        free(err);
        return 0;
    }
    free(err);

    struct json_object *root = json_tokener_parse(reply);
    free(reply);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root)
            json_object_put(root);
        return 0;
    }

    json_object_object_foreach(root, monitor_name, monitor_obj) {
        (void)monitor_name;
        struct json_object *levels = NULL;
        if (!json_object_is_type(monitor_obj, json_type_object) ||
            !json_object_object_get_ex(monitor_obj, "levels", &levels) ||
            !json_object_is_type(levels, json_type_object))
            continue;

        json_object_object_foreach(levels, level_name, arr) {
            (void)level_name;
            if (!json_object_is_type(arr, json_type_array))
                continue;
            int n = json_object_array_length(arr);
            for (int i = 0; i < n; i++) {
                struct json_object *el = json_object_array_get_idx(arr, i);
                if (!el || !json_object_is_type(el, json_type_object))
                    continue;
                if (parse_app_object(el, list, 1) < 0) {
                    json_object_put(root);
                    return -1;
                }
            }
        }
    }

    json_object_put(root);
    return 0;
}

static int collect_children(struct AppList *list, pid_t compositor_pid) {
    pid_t pids[512];
    int n;

    if (compositor_pid <= 0)
        return 0;

    n = proc_collect_children(compositor_pid, pids, 512);
    for (int i = 0; i < n; i++) {
        char name[64];
        if (proc_comm(pids[i], name, sizeof(name)) < 0)
            continue;
        if (apps_add(list, NULL, name, "", pids[i], 0, 1) < 0)
            return -1;
    }
    return 0;
}

int apps_collect(struct AppList *list, const struct Hyprland *hl) {
    memset(list, 0, sizeof(*list));
    if (collect_clients(list) < 0)
        return -1;
    if (collect_layers(list) < 0)
        return -1;
    if (collect_children(list, hl->compositor_pid) < 0)
        return -1;
    return 0;
}

static int dispatch_close(const struct App *app, int use_lua) {
    char *cmd = NULL;
    char *err = NULL;
    char *reply = NULL;
    int rc = 0;

    if (app->always_signal || !app->address || app->address[0] == '\0') {
        if (app->pid <= 0)
            return 0;
        if (kill(app->pid, SIGTERM) != 0 && errno != ESRCH) {
            fprintf(stderr, "hyprdown: SIGTERM %s (pid %d): %s\n",
                    app->class_name, (int)app->pid, strerror(errno));
            return -1;
        }
        return 0;
    }

    if (use_lua) {
        if (asprintf(&cmd, "/dispatch hl.dsp.window.close({ window = 'address:%s' })",
                     app->address) < 0)
            return -1;
    } else {
        if (asprintf(&cmd, "/dispatch closewindow address:%s", app->address) < 0)
            return -1;
    }

    reply = hypr_request(cmd, &err);
    if (!reply) {
        fprintf(stderr, "hyprdown: close %s: %s\n", app->class_name,
                err ? err : "IPC error");
        rc = -1;
    } else if (strcmp(reply, "ok") != 0 && strncmp(reply, "ok", 2) != 0) {
        /* Hyprland may already have closed the window */
    }

    free(cmd);
    free(err);
    free(reply);
    return rc;
}

void apps_quit(const struct AppList *list, int use_lua) {
    for (size_t i = 0; i < list->count; i++)
        dispatch_close(&list->items[i], use_lua);
}

void apps_kill(const struct AppList *list) {
    for (size_t i = 0; i < list->count; i++) {
        pid_t pid = list->items[i].pid;
        if (pid <= 1 || pid == getpid())
            continue;
        if (!proc_alive(pid))
            continue;
        if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
            fprintf(stderr, "hyprdown: SIGKILL %s (pid %d): %s\n",
                    list->items[i].class_name, (int)pid, strerror(errno));
        }
    }
}

static int address_in_clients(struct json_object *clients, const char *address) {
    if (!address || !clients)
        return 0;
    int n = json_object_array_length(clients);
    for (int i = 0; i < n; i++) {
        struct json_object *el = json_object_array_get_idx(clients, i);
        const char *addr = NULL;
        if (!el || !json_get_string(el, "address", &addr) || !addr)
            continue;
        if (strcmp(addr, address) == 0)
            return 1;
    }
    return 0;
}

static int pid_has_window(struct json_object *clients, pid_t pid) {
    if (pid <= 0 || !clients)
        return 0;
    int n = json_object_array_length(clients);
    for (int i = 0; i < n; i++) {
        struct json_object *el = json_object_array_get_idx(clients, i);
        int64_t p = 0;
        if (!el || !json_get_int64(el, "pid", &p))
            continue;
        if ((pid_t)p == pid)
            return 1;
    }
    return 0;
}

int apps_refresh(struct AppList *list) {
    char *err = NULL;
    char *reply = hypr_request("j/clients", &err);
    struct json_object *clients = NULL;
    size_t w = 0;

    if (!reply) {
        /* compositor may already be going away */
        free(err);
        for (size_t i = 0; i < list->count; i++) {
            free(list->items[i].address);
            free(list->items[i].class_name);
            free(list->items[i].title);
        }
        list->count = 0;
        return 0;
    }
    free(err);

    clients = json_tokener_parse(reply);
    free(reply);
    if (!clients || !json_object_is_type(clients, json_type_array)) {
        if (clients)
            json_object_put(clients);
        return -1;
    }

    for (size_t i = 0; i < list->count; i++) {
        struct App *app = &list->items[i];
        int in_clients = app->address && address_in_clients(clients, app->address);
        int alive = proc_alive(app->pid);

        if (!in_clients && alive && app->pid > 0 && app->address &&
            app->address[0] != '\0' && !pid_has_window(clients, app->pid)) {
            /* window gone but process still running */
            if (kill(app->pid, SIGTERM) != 0 && errno != ESRCH) {
                fprintf(stderr, "hyprdown: SIGTERM leftover %s (pid %d): %s\n",
                        app->class_name, (int)app->pid, strerror(errno));
            }
        }

        if (!in_clients && !alive) {
            free(app->address);
            free(app->class_name);
            free(app->title);
            continue;
        }

        if (w != i)
            list->items[w] = *app;
        w++;
    }

    list->count = w;
    json_object_put(clients);
    return 0;
}

static double monotonic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void print_remaining(const struct AppList *list, double left) {
    fprintf(stderr, "hyprdown: waiting for %zu app(s)", list->count);
    if (left >= 0)
        fprintf(stderr, " (%.1fs left)", left);
    fputc('\n', stderr);

    size_t shown = list->count < 8 ? list->count : 8;
    for (size_t i = 0; i < shown; i++) {
        const struct App *app = &list->items[i];
        fprintf(stderr, "  - %s", app->class_name ? app->class_name : "unknown");
        if (app->title && app->title[0])
            fprintf(stderr, " (%s)", app->title);
        fputc('\n', stderr);
    }
    if (list->count > shown)
        fprintf(stderr, "  - ...\n");
}

int apps_wait(struct AppList *list, unsigned timeout_sec, int use_lua) {
    const double start = monotonic_now();
    const double deadline = start + (double)timeout_sec;
    double last_print = 0;
    double last_reclose = start;

    if (list->count == 0)
        return 0;

    print_remaining(list, (double)timeout_sec);
    last_print = start;

    while (list->count > 0) {
        double now = monotonic_now();
        if (now >= deadline)
            break;

        if (now - last_reclose >= 1.0) {
            apps_quit(list, use_lua);
            last_reclose = now;
        }

        if (apps_refresh(list) < 0)
            return -1;

        now = monotonic_now();
        if (list->count > 0 && now - last_print >= 1.0) {
            print_remaining(list, deadline - now);
            last_print = now;
        }

        if (list->count == 0)
            break;

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    if (list->count == 0) {
        fprintf(stderr, "hyprdown: all apps exited\n");
        return 0;
    }

    fprintf(stderr, "hyprdown: timeout after %us, %zu app(s) still running\n",
            timeout_sec, list->count);
    apps_kill(list);
    return 1;
}
