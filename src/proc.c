#include "hyprdown.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Parse /proc/<pid>/stat. comm may contain spaces and parentheses, so fields
 * are read after the last ')'. */
int proc_info(pid_t pid, struct ProcInfo *out) {
    char path[64];
    char buf[1024];
    struct stat st;
    FILE *fp;
    size_t n;

    if (pid <= 0 || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    if (stat(path, &st) < 0)
        return -1;
    out->uid = st.st_uid;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0)
        return -1;
    buf[n] = '\0';

    char *open = strchr(buf, '(');
    char *close = strrchr(buf, ')');
    if (!open || !close || close < open)
        return -1;

    size_t comm_len = (size_t)(close - open - 1);
    if (comm_len >= sizeof(out->comm))
        comm_len = sizeof(out->comm) - 1;
    memcpy(out->comm, open + 1, comm_len);
    out->comm[comm_len] = '\0';

    /* Fields after comm: 3 state, 4 ppid, ..., 22 starttime. */
    char *p = close + 1;
    int field = 3;
    char *save = NULL;
    for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        if (field == 3)
            out->state = tok[0];
        else if (field == 4)
            out->ppid = (pid_t)strtol(tok, NULL, 10);
        else if (field == 22) {
            out->starttime = strtoull(tok, NULL, 10);
            return 0;
        }
    }

    return -1;
}

int proc_collect_children(pid_t parent, pid_t *out, size_t cap) {
    DIR *dir;
    struct dirent *ent;
    size_t n = 0;

    if (parent <= 0 || !out || cap == 0)
        return 0;

    dir = opendir("/proc");
    if (!dir)
        return 0;

    while (n < cap && (ent = readdir(dir)) != NULL) {
        struct ProcInfo info;
        char *end = NULL;

        if (!isdigit((unsigned char)ent->d_name[0]))
            continue;

        pid_t pid = (pid_t)strtol(ent->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0)
            continue;
        if (proc_info(pid, &info) < 0 || info.ppid != parent)
            continue;
        out[n++] = pid;
    }

    closedir(dir);
    return (int)n;
}

/* cgroup v2 path of pid, e.g. "/user.slice/user-1000.slice/user@1000.service/...". */
static int proc_cgroup(pid_t pid, char *buf, size_t buflen) {
    char path[64];
    char line[1024];
    int ok = -1;

    snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "0::", 3) != 0)
            continue;
        size_t len = strcspn(line + 3, "\n");
        if (len >= buflen)
            len = buflen - 1;
        memcpy(buf, line + 3, len);
        buf[len] = '\0';
        ok = 0;
        break;
    }

    fclose(fp);
    return ok;
}

/* Find the app unit pid belongs to: an "app-*" scope or service in the user
 * manager's app.slice, which is how uwsm, app2unit, XDG autostart, Flatpak and
 * Chromium-based apps run applications. unit receives the unit's cgroup path. */
static int app_unit_of(pid_t pid, const char *manager, char *unit, size_t len) {
    char cgroup[1024];
    char *unit_start = NULL;
    char *unit_end = NULL;

    if (proc_cgroup(pid, cgroup, sizeof(cgroup)) < 0)
        return 0;
    char *m = strstr(cgroup, manager);
    if (!m || strncmp(m + strlen(manager), "app.slice/", 10) != 0)
        return 0;

    /* The unit is the innermost .scope or .service; slices may nest above it. */
    for (char *c = m + strlen(manager);;) {
        char *slash = strchr(c, '/');
        size_t clen = slash ? (size_t)(slash - c) : strlen(c);
        if ((clen > 6 && strncmp(c + clen - 6, ".scope", 6) == 0) ||
            (clen > 8 && strncmp(c + clen - 8, ".service", 8) == 0)) {
            unit_start = c;
            unit_end = c + clen;
        }
        if (!slash)
            break;
        c = slash + 1;
    }
    if (!unit_start || strncmp(unit_start, "app-", 4) != 0)
        return 0;

    *unit_end = '\0';
    snprintf(unit, len, "%s", cgroup);
    return 1;
}

struct UnitMain {
    char *unit;
    unsigned long long starttime;
    pid_t pid;
};

/* One process per app unit: the earliest started one whose parent is not part
 * of any app unit, which is the app itself. A terminal's shells, a browser's
 * helpers, and the zygote a Chromium app leaves behind when it moves itself
 * into a scope of its own are all left to the app that started them. */
int proc_collect_app_units(pid_t *out, size_t cap) {
    char manager[64];
    char unit[1024];
    char parent_unit[1024];
    struct UnitMain *units = NULL;
    size_t nunits = 0;
    size_t capunits = 0;
    DIR *dir;
    struct dirent *ent;
    size_t n = 0;

    if (!out || cap == 0)
        return 0;

    snprintf(manager, sizeof(manager), "/user@%u.service/", (unsigned)getuid());
    dir = opendir("/proc");
    if (!dir)
        return 0;

    while ((ent = readdir(dir)) != NULL) {
        struct ProcInfo info;
        char *end = NULL;

        if (!isdigit((unsigned char)ent->d_name[0]))
            continue;

        pid_t pid = (pid_t)strtol(ent->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 1)
            continue;
        if (proc_info(pid, &info) < 0 || info.uid != getuid())
            continue;
        if (!app_unit_of(pid, manager, unit, sizeof(unit)) ||
            app_unit_of(info.ppid, manager, parent_unit, sizeof(parent_unit)))
            continue;

        size_t i = 0;
        while (i < nunits && strcmp(units[i].unit, unit) != 0)
            i++;
        if (i < nunits) {
            if (info.starttime < units[i].starttime) {
                units[i].starttime = info.starttime;
                units[i].pid = pid;
            }
            continue;
        }

        if (nunits == capunits) {
            size_t ncap = capunits ? capunits * 2 : 32;
            struct UnitMain *grown = realloc(units, ncap * sizeof(*units));
            if (!grown)
                break;
            units = grown;
            capunits = ncap;
        }
        char *copy = strdup(unit);
        if (!copy)
            break;
        units[nunits++] = (struct UnitMain){.unit = copy, .starttime = info.starttime, .pid = pid};
    }
    closedir(dir);

    for (size_t i = 0; i < nunits; i++) {
        if (n < cap)
            out[n++] = units[i].pid;
        free(units[i].unit);
    }
    free(units);
    return (int)n;
}
