#include "hyprdown.h"

#include <errno.h>
#include <json-c/json.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static uid_t effective_uid(void) {
    const struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_uid : getuid();
}

char *hypr_socket_path(void) {
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char *path = NULL;

    if (!his || his[0] == '\0')
        return NULL;

    if (xdg && xdg[0] != '\0') {
        if (asprintf(&path, "%s/hypr/%s/.socket.sock", xdg, his) < 0)
            return NULL;
    } else {
        if (asprintf(&path, "/run/user/%u/hypr/%s/.socket.sock",
                     (unsigned)effective_uid(), his) < 0)
            return NULL;
    }

    return path;
}

static char *hypr_runtime_dir(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char *path = NULL;

    if (xdg && xdg[0] != '\0') {
        if (asprintf(&path, "%s/hypr", xdg) < 0)
            return NULL;
    } else {
        if (asprintf(&path, "/run/user/%u/hypr", (unsigned)effective_uid()) < 0)
            return NULL;
    }
    return path;
}

char *hypr_request(const char *cmd, char **err) {
    char *path = hypr_socket_path();
    int fd = -1;
    struct sockaddr_un addr;
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    char *reply = NULL;
    size_t reply_len = 0;
    size_t reply_cap = 0;

    if (err)
        *err = NULL;

    if (!path) {
        if (err)
            *err = strdup("HYPRLAND_INSTANCE_SIGNATURE is unset; not running under Hyprland");
        return NULL;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (err)
            asprintf(err, "failed to create socket: %s", strerror(errno));
        free(path);
        return NULL;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        /* timeouts are best-effort */
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        if (err)
            asprintf(err, "Hyprland socket path is too long: %s", path);
        goto fail;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (err)
            asprintf(err, "failed to connect to %s: %s", path, strerror(errno));
        goto fail;
    }

    size_t cmd_len = strlen(cmd);
    ssize_t wrote = write(fd, cmd, cmd_len);
    if (wrote < 0 || (size_t)wrote != cmd_len) {
        if (err)
            asprintf(err, "failed to write to Hyprland socket: %s", strerror(errno));
        goto fail;
    }

    /* Hyprland leaves the command socket open after a reply. hyprctl/hyprshutdown
     * stop after a short read; waiting for EOF would block until SO_RCVTIMEO. */
    for (;;) {
        char buf[8192];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (err)
                    *err = strdup("Hyprland IPC did not respond in time");
            } else if (err) {
                asprintf(err, "failed to read from Hyprland socket: %s", strerror(errno));
            }
            goto fail;
        }
        if (n == 0)
            break;

        if (reply_len + (size_t)n + 1 > reply_cap) {
            size_t ncap = reply_cap ? reply_cap * 2 : 8192;
            while (ncap < reply_len + (size_t)n + 1)
                ncap *= 2;
            char *nreply = realloc(reply, ncap);
            if (!nreply) {
                if (err)
                    *err = strdup("out of memory");
                goto fail;
            }
            reply = nreply;
            reply_cap = ncap;
        }
        memcpy(reply + reply_len, buf, (size_t)n);
        reply_len += (size_t)n;
        reply[reply_len] = '\0';

        if ((size_t)n < sizeof(buf))
            break;
    }

    close(fd);
    free(path);
    if (!reply) {
        reply = strdup("");
    }
    return reply;

fail:
    if (fd >= 0)
        close(fd);
    free(path);
    free(reply);
    return NULL;
}

int hypr_detect_lua(void) {
    char *err = NULL;
    char *reply = hypr_request("j/status", &err);
    int use_lua = 0;

    free(err);
    if (!reply)
        return 0;

    struct json_object *root = json_tokener_parse(reply);
    free(reply);
    if (!root)
        return 0;

    struct json_object *prov = NULL;
    if (json_object_object_get_ex(root, "configProvider", &prov) &&
        json_object_is_type(prov, json_type_string)) {
        const char *s = json_object_get_string(prov);
        if (s && strcmp(s, "lua") == 0)
            use_lua = 1;
    }

    json_object_put(root);
    return use_lua;
}

int hypr_compositor_pid(pid_t *out_pid) {
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    char *dir = NULL;
    char *lock_path = NULL;
    FILE *fp = NULL;
    char line[64];
    long pid = -1;
    int ok = -1;

    if (!his || his[0] == '\0' || !out_pid)
        return -1;

    dir = hypr_runtime_dir();
    if (!dir)
        return -1;

    if (asprintf(&lock_path, "%s/%s/hyprland.lock", dir, his) < 0) {
        free(dir);
        return -1;
    }

    fp = fopen(lock_path, "r");
    if (!fp)
        goto done;

    if (!fgets(line, sizeof(line), fp))
        goto done;

    errno = 0;
    pid = strtol(line, NULL, 10);
    if (errno != 0 || pid <= 0)
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
