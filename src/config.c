#include "hyprdown.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static int mkdir_p(const char *path, mode_t mode) {
    char *tmp;
    size_t len;

    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    tmp = xstrdup(path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
            free(tmp);
            return -1;
        }
        *p = '/';
    }

    if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

static const char *home_dir(void) {
    const char *home = getenv("HOME");
    if (home && home[0] != '\0')
        return home;

    const struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] != '\0')
        return pw->pw_dir;
    return NULL;
}

static char *config_dir_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *path = NULL;

    if (xdg && xdg[0] != '\0') {
        if (asprintf(&path, "%s/hyprdown", xdg) < 0)
            return NULL;
        return path;
    }

    const char *home = home_dir();
    if (!home)
        return NULL;
    if (asprintf(&path, "%s/.config/hyprdown", home) < 0)
        return NULL;
    return path;
}

static char *config_file_path(const char *dir) {
    char *path = NULL;
    if (!dir)
        return NULL;
    if (asprintf(&path, "%s/config.toml", dir) < 0)
        return NULL;
    return path;
}

static int ensure_config_file(char **out_path) {
    char *dir;
    char *file;
    int fd;

    *out_path = NULL;
    dir = config_dir_path();
    if (!dir) {
        fprintf(stderr, "hyprdown: warning: cannot determine config directory "
                        "(set XDG_CONFIG_HOME or HOME)\n");
        return -1;
    }

    if (mkdir_p(dir, 0700) < 0) {
        fprintf(stderr, "hyprdown: warning: could not create %s: %s\n", dir,
                strerror(errno));
        free(dir);
        return -1;
    }

    {
        struct stat st;
        if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "hyprdown: warning: %s is not a directory\n", dir);
            free(dir);
            return -1;
        }
    }

    file = config_file_path(dir);
    free(dir);
    if (!file) {
        fprintf(stderr, "hyprdown: out of memory\n");
        return -1;
    }

    fd = open(file, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (fd < 0) {
        if (errno != EEXIST) {
            fprintf(stderr, "hyprdown: warning: could not create %s: %s\n", file,
                    strerror(errno));
            free(file);
            return -1;
        }
    } else {
        close(fd);
    }

    *out_path = file;
    return 0;
}

static char *trim(char *s) {
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                       end[-1] == '\r'))
        end--;
    *end = '\0';
    return s;
}

static int skip_ws(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    *pp = p;
    return *p != '\0';
}

static int parse_double_quoted(const char **pp, char **out, char **err) {
    const char *p = *pp;
    size_t cap = 32;
    size_t n = 0;
    char *buf;

    if (*p != '"')
        return -1;
    p++;

    buf = malloc(cap);
    if (!buf) {
        *err = xstrdup("out of memory");
        return -1;
    }

    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (*p == '\0') {
                free(buf);
                *err = xstrdup("unterminated string");
                return -1;
            }
            switch (*p++) {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case 'r':
                c = '\r';
                break;
            default:
                free(buf);
                *err = xstrdup("unknown string escape");
                return -1;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *nbuf = realloc(buf, cap);
            if (!nbuf) {
                free(buf);
                *err = xstrdup("out of memory");
                return -1;
            }
            buf = nbuf;
        }
        buf[n++] = c;
    }

    if (*p != '"') {
        free(buf);
        *err = xstrdup("unterminated string");
        return -1;
    }
    p++;
    buf[n] = '\0';
    *out = buf;
    *pp = p;
    return 0;
}

static int parse_single_quoted(const char **pp, char **out, char **err) {
    const char *p = *pp;
    const char *start;
    size_t n;

    if (*p != '\'')
        return -1;
    p++;
    start = p;
    while (*p && *p != '\'')
        p++;
    if (*p != '\'') {
        *err = xstrdup("unterminated string");
        return -1;
    }
    n = (size_t)(p - start);
    *out = malloc(n + 1);
    if (!*out) {
        *err = xstrdup("out of memory");
        return -1;
    }
    memcpy(*out, start, n);
    (*out)[n] = '\0';
    *pp = p + 1;
    return 0;
}

static int parse_uint(const char **pp, unsigned *out, char **err) {
    const char *p = *pp;
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(p, &end, 10);
    if (errno != 0 || end == p) {
        *err = xstrdup("invalid integer");
        return -1;
    }
    if (v > (unsigned long)INT_MAX) {
        *err = xstrdup("integer out of range");
        return -1;
    }
    *out = (unsigned)v;
    *pp = end;
    return 0;
}

static int set_string_key(char **slot, char *value) {
    free(*slot);
    if (!value || value[0] == '\0') {
        free(value);
        *slot = NULL;
        return 0;
    }
    *slot = value;
    return 0;
}

static int parse_line(char *line, int lineno, const char *path, struct Config *cfg) {
    char *hash;
    char *key;
    char *eq;
    const char *valp;
    char *err = NULL;
    int in_str = 0;
    char quote = 0;

    /* Strip unquoted comments. */
    for (hash = line; *hash; hash++) {
        if (!in_str && (*hash == '"' || *hash == '\'')) {
            in_str = 1;
            quote = *hash;
        } else if (in_str && *hash == quote && (quote == '\'' || hash == line || hash[-1] != '\\')) {
            in_str = 0;
        } else if (!in_str && *hash == '#') {
            *hash = '\0';
            break;
        }
    }

    line = trim(line);
    if (line[0] == '\0')
        return 0;

    if (line[0] == '[') {
        fprintf(stderr, "hyprdown: %s:%d: tables are not supported\n", path, lineno);
        return -1;
    }

    eq = strchr(line, '=');
    if (!eq) {
        fprintf(stderr, "hyprdown: %s:%d: expected key = value\n", path, lineno);
        return -1;
    }
    *eq = '\0';
    key = trim(line);
    valp = eq + 1;
    skip_ws(&valp);

    if (key[0] == '\0') {
        fprintf(stderr, "hyprdown: %s:%d: missing key\n", path, lineno);
        return -1;
    }
    if (*valp == '\0') {
        fprintf(stderr, "hyprdown: %s:%d: missing value\n", path, lineno);
        return -1;
    }

    if (strcmp(key, "timeout") == 0) {
        unsigned v = 0;
        if (parse_uint(&valp, &v, &err) < 0)
            goto bad_value;
        skip_ws(&valp);
        if (*valp != '\0') {
            err = xstrdup("unexpected trailing characters");
            goto bad_value;
        }
        cfg->timeout_sec = v;
        cfg->has_timeout = 1;
        return 0;
    }

    if (strcmp(key, "poweroff_override") == 0 ||
        strcmp(key, "reboot_override") == 0 ||
        strcmp(key, "logout_override") == 0) {
        char *value = NULL;
        if (*valp == '"') {
            if (parse_double_quoted(&valp, &value, &err) < 0)
                goto bad_value;
        } else if (*valp == '\'') {
            if (parse_single_quoted(&valp, &value, &err) < 0)
                goto bad_value;
        } else {
            err = xstrdup("string value must be quoted");
            goto bad_value;
        }
        skip_ws(&valp);
        if (*valp != '\0') {
            free(value);
            err = xstrdup("unexpected trailing characters");
            goto bad_value;
        }
        if (strcmp(key, "poweroff_override") == 0)
            return set_string_key(&cfg->poweroff_override, value);
        if (strcmp(key, "reboot_override") == 0)
            return set_string_key(&cfg->reboot_override, value);
        return set_string_key(&cfg->logout_override, value);
    }

    fprintf(stderr, "hyprdown: %s:%d: unknown key '%s'\n", path, lineno, key);
    return -1;

bad_value:
    fprintf(stderr, "hyprdown: %s:%d: %s\n", path, lineno, err ? err : "invalid value");
    free(err);
    return -1;
}

static int load_config(const char *path, struct Config *cfg) {
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    int lineno = 0;
    int rc = 0;

    fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT)
            return 0;
        fprintf(stderr, "hyprdown: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }

    while (getline(&line, &cap, fp) >= 0) {
        lineno++;
        if (parse_line(line, lineno, path, cfg) < 0) {
            rc = -1;
            break;
        }
    }

    if (rc == 0 && ferror(fp)) {
        fprintf(stderr, "hyprdown: error reading %s: %s\n", path, strerror(errno));
        rc = -1;
    }

    free(line);
    fclose(fp);
    return rc;
}

void config_free(struct Config *cfg) {
    if (!cfg)
        return;
    free(cfg->poweroff_override);
    free(cfg->reboot_override);
    free(cfg->logout_override);
    memset(cfg, 0, sizeof(*cfg));
}

int config_init(struct Config *cfg) {
    char *path = NULL;
    int rc;

    memset(cfg, 0, sizeof(*cfg));
    cfg->timeout_sec = HYPRDOWN_DEFAULT_TIMEOUT;

    if (ensure_config_file(&path) < 0)
        return 0;

    rc = load_config(path, cfg);
    if (rc < 0)
        config_free(cfg);
    free(path);
    return rc;
}
