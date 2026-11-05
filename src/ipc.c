#include "hyprdown.h"

#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define IPC_TIMEOUT_MS 5000

static char *hypr_instance_dir(void) {
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char *path = NULL;
    int rc;

    if (!his || his[0] == '\0')
        return NULL;

    if (xdg && xdg[0] != '\0')
        rc = asprintf(&path, "%s/hypr/%s", xdg, his);
    else
        rc = asprintf(&path, "/run/user/%u/hypr/%s", (unsigned)getuid(), his);

    return rc < 0 ? NULL : path;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

__attribute__((format(printf, 2, 3)))
static void set_err(char **err, const char *fmt, ...) {
    va_list ap;

    if (!err)
        return;
    va_start(ap, fmt);
    if (vasprintf(err, fmt, ap) < 0)
        *err = NULL;
    va_end(ap);
}

/* Hyprland writes the full reply and then closes the connection, so the reply
 * is read until EOF. A short read does not mean the reply is complete. */
char *hypr_request(const char *cmd, char **err) {
    char *dir = hypr_instance_dir();
    char *path = NULL;
    struct sockaddr_un addr;
    int fd = -1;
    char *reply = NULL;
    size_t reply_len = 0;
    size_t reply_cap = 0;

    if (err)
        *err = NULL;

    if (!dir || asprintf(&path, "%s/.socket.sock", dir) < 0) {
        free(dir);
        set_err(err, "HYPRLAND_INSTANCE_SIGNATURE is unset");
        return NULL;
    }
    free(dir);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        set_err(err, "socket path is too long: %s", path);
        goto fail;
    }
    strcpy(addr.sun_path, path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        set_err(err, "socket: %s", strerror(errno));
        goto fail;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        set_err(err, "connect %s: %s", path, strerror(errno));
        goto fail;
    }

    size_t cmd_len = strlen(cmd);
    size_t sent = 0;
    while (sent < cmd_len) {
        ssize_t w = send(fd, cmd + sent, cmd_len - sent, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            set_err(err, "write: %s", strerror(errno));
            goto fail;
        }
        sent += (size_t)w;
    }

    const long long deadline = now_ms() + IPC_TIMEOUT_MS;
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) {
            set_err(err, "Hyprland IPC did not respond in time");
            goto fail;
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, (int)left);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            set_err(err, "poll: %s", strerror(errno));
            goto fail;
        }
        if (pr == 0)
            continue;

        if (reply_cap - reply_len < 8192 + 1) {
            size_t ncap = reply_cap ? reply_cap * 2 : 16384;
            char *nreply = realloc(reply, ncap);
            if (!nreply) {
                set_err(err, "out of memory");
                goto fail;
            }
            reply = nreply;
            reply_cap = ncap;
        }

        ssize_t n = read(fd, reply + reply_len, 8192);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            set_err(err, "read: %s", strerror(errno));
            goto fail;
        }
        if (n == 0)
            break;
        reply_len += (size_t)n;
    }

    close(fd);
    free(path);
    if (!reply)
        return strdup("");
    reply[reply_len] = '\0';
    return reply;

fail:
    if (fd >= 0)
        close(fd);
    free(path);
    free(reply);
    return NULL;
}

/* Hyprland 0.55+ can run a Lua config, which replaces the hyprlang
 * dispatchers. `j/status` reports which provider is active. */
int hypr_detect_lua(void) {
    char *reply = hypr_request("j/status", NULL);
    int use_lua = 0;

    if (!reply)
        return 0;

    struct json_object *root = json_tokener_parse(reply);
    free(reply);
    if (!root)
        return 0;

    struct json_object *prov = NULL;
    if (json_object_object_get_ex(root, "configProvider", &prov) &&
        json_object_is_type(prov, json_type_string) &&
        strcmp(json_object_get_string(prov), "lua") == 0)
        use_lua = 1;

    json_object_put(root);
    return use_lua;
}

/* hyprland.lock holds the compositor pid on its first line. */
int hypr_compositor_pid(pid_t *out_pid) {
    char *dir = hypr_instance_dir();
    char *lock_path = NULL;
    FILE *fp = NULL;
    char line[64];
    int ok = -1;

    if (!dir || !out_pid)
        goto done;
    if (asprintf(&lock_path, "%s/hyprland.lock", dir) < 0) {
        lock_path = NULL;
        goto done;
    }

    fp = fopen(lock_path, "r");
    if (!fp || !fgets(line, sizeof(line), fp))
        goto done;

    char *end = NULL;
    long pid = strtol(line, &end, 10);
    if (end == line || pid <= 1)
        goto done;

    /* Guard against a stale lock: the pid must still be Hyprland. Nix wraps
     * the binary, so its comm is ".Hyprland-wrapp". */
    struct ProcInfo info;
    if (proc_info((pid_t)pid, &info) < 0 || !strcasestr(info.comm, "hyprland"))
        goto done;

    *out_pid = (pid_t)pid;
    ok = 0;

done:
    if (fp)
        fclose(fp);
    free(lock_path);
    free(dir);
    return ok;
}

/* Returns NULL when Hyprland accepted the command, otherwise why it did not. */
static char *hypr_dispatch(const char *cmd) {
    char *err = NULL;
    char *reply = hypr_request(cmd, &err);

    if (!reply)
        return err ? err : strdup("IPC error");

    free(err);
    if (strncmp(reply, "ok", 2) == 0) {
        free(reply);
        return NULL;
    }
    return reply;
}

char *hypr_close_window(const char *address, int use_lua) {
    char *cmd = NULL;
    int rc;

    if (use_lua)
        rc = asprintf(&cmd, "/dispatch hl.dsp.window.close({ window = 'address:%s' })", address);
    else
        rc = asprintf(&cmd, "/dispatch closewindow address:%s", address);
    if (rc < 0)
        return strdup("out of memory");

    char *failure = hypr_dispatch(cmd);
    free(cmd);
    return failure;
}

int hypr_exit(int use_lua) {
    char *failure = hypr_dispatch(use_lua ? "/dispatch hl.dsp.exit()" : "/dispatch exit");

    if (!failure)
        return 0;
    fprintf(stderr, "hyprdown: exit Hyprland: %s\n", failure);
    free(failure);
    return -1;
}

/* hyprdown usually runs from a keybind or menu, where stderr goes nowhere. */
void hypr_notify_error(const char *message) {
    char *cmd = NULL;

    fprintf(stderr, "hyprdown: %s\n", message);
    if (asprintf(&cmd, "/notify 3 10000 0 hyprdown: %s", message) < 0)
        return;
    free(hypr_request(cmd, NULL));
    free(cmd);
}
