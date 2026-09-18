#ifndef HYPRDOWN_H
#define HYPRDOWN_H

#include <stddef.h>
#include <sys/types.h>

#ifndef HYPRDOWN_VERSION
#define HYPRDOWN_VERSION "0.2.0"
#endif

#define HYPRDOWN_DEFAULT_TIMEOUT 5

enum Action {
    ACTION_NONE = 0,
    ACTION_SHUTDOWN,
    ACTION_REBOOT,
    ACTION_LOGOUT,
    ACTION_COMMAND
};

struct Options {
    enum Action action;
    unsigned timeout_sec;
    char *command;
};

struct Config {
    char *poweroff_override;
    char *reboot_override;
    char *logout_override;
    unsigned timeout_sec;
    int has_timeout;
};

struct App {
    char *address;
    char *class_name;
    char *title;
    pid_t pid;
    int is_layer;
    int always_signal;
};

struct AppList {
    struct App *items;
    size_t count;
    size_t cap;
};

struct Hyprland {
    int use_lua;
    pid_t compositor_pid;
};

/* args / usage */
void print_help(unsigned default_timeout);
void print_version(void);
int parse_args(int argc, char **argv, struct Options *opt, unsigned default_timeout);

/* config: $XDG_CONFIG_HOME/hyprdown/config.toml or $HOME/.config/hyprdown/config.toml */
int config_init(struct Config *cfg);
void config_free(struct Config *cfg);

/* Hyprland IPC */
char *hypr_socket_path(void);
char *hypr_request(const char *cmd, char **err);
int hypr_detect_lua(void);
int hypr_compositor_pid(pid_t *out_pid);

/* apps */
void apps_free(struct AppList *list);
int apps_collect(struct AppList *list, const struct Hyprland *hl);
void apps_quit(const struct AppList *list, int use_lua);
void apps_kill(const struct AppList *list);
int apps_refresh(struct AppList *list);
int apps_wait(struct AppList *list, unsigned timeout_sec, int use_lua);

/* power / session actions */
int run_action(enum Action action, const char *command, const struct Config *cfg);

/* process helpers */
int proc_alive(pid_t pid);
pid_t proc_ppid(pid_t pid);
int proc_comm(pid_t pid, char *buf, size_t buflen);
int proc_collect_children(pid_t parent, pid_t *out, size_t cap);

#endif
