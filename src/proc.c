#include "hyprdown.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int proc_alive(pid_t pid) {
    if (pid <= 0)
        return 0;
    if (kill(pid, 0) == 0)
        return 1;
    return errno == EPERM;
}

pid_t proc_ppid(pid_t pid) {
    char path[64];
    FILE *fp;
    char line[256];
    pid_t ppid = -1;

    if (pid <= 0)
        return -1;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "PPid:", 5) != 0)
            continue;
        ppid = (pid_t)strtol(line + 5, NULL, 10);
        break;
    }

    fclose(fp);
    return ppid;
}

int proc_comm(pid_t pid, char *buf, size_t buflen) {
    char path[64];
    FILE *fp;
    char line[256];
    int ok = -1;

    if (!buf || buflen == 0 || pid <= 0)
        return -1;

    buf[0] = '\0';
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Name:", 5) != 0)
            continue;
        char *name = line + 5;
        while (*name == ' ' || *name == '\t')
            name++;
        size_t len = strcspn(name, "\r\n");
        if (len >= buflen)
            len = buflen - 1;
        memcpy(buf, name, len);
        buf[len] = '\0';
        ok = 0;
        break;
    }

    fclose(fp);
    return ok;
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

    while ((ent = readdir(dir)) != NULL) {
        pid_t pid;
        char *end = NULL;

        if (!isdigit((unsigned char)ent->d_name[0]))
            continue;

        pid = (pid_t)strtol(ent->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0)
            continue;
        if (proc_ppid(pid) != parent)
            continue;
        if (n < cap)
            out[n] = pid;
        n++;
        if (n >= cap)
            break;
    }

    closedir(dir);
    return (int)n;
}
