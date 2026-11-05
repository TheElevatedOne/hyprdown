#include "hyprdown.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
