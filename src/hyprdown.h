#ifndef HYPRDOWN_H
#define HYPRDOWN_H

#include <stddef.h>
#include <sys/types.h>

#ifndef HYPRDOWN_VERSION
#define HYPRDOWN_VERSION "0.3.0"
#endif

#define HYPRDOWN_DEFAULT_TIMEOUT 5
#define HYPRDOWN_MAX_PROTECTED 64

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
    int no_fork;
    int dry_run;
};

struct Config {
    char *poweroff_override;
    char *reboot_override;
    char *logout_override;
    unsigned timeout_sec;
    int has_timeout;
};

/* A Hyprland client window, closed through the compositor. */
struct Window {
    char *address;
    char *class_name;
    char *title;
    pid_t pid;
    int present;
    int warned;
};

/* A process that must exit: window owners, layer-shell clients and direct
 * children of the compositor. Identified by pid + start time so a reused pid
 * is never signalled. */
struct Proc {
    pid_t pid;
    unsigned long long starttime;
    char *name;
    int termed;
    int alive;
    int had_window;
    double windows_gone_at;
};

struct Session {
    int use_lua;
    pid_t compositor_pid;
    pid_t protected_pids[HYPRDOWN_MAX_PROTECTED];
    size_t nprotected;

    struct Window *windows;
    size_t nwindows;
    size_t capwindows;

    struct Proc *procs;
    size_t nprocs;
    size_t capprocs;
};

struct ProcInfo {
    pid_t ppid;
    uid_t uid;
    char state;
    unsigned long long starttime;
    char comm[64];
};

/* args / usage */
void print_help(unsigned default_timeout);
void print_version(void);
int parse_args(int argc, char **argv, struct Options *opt, unsigned default_timeout);

/* config: $XDG_CONFIG_HOME/hyprdown/config.toml or $HOME/.config/hyprdown/config.toml */
int config_init(struct Config *cfg);
void config_free(struct Config *cfg);
/* opens $XDG_STATE_HOME/hyprdown/hyprdown.log or $HOME/.local/state/hyprdown/hyprdown.log */
int log_open(char **out_path);

/* Hyprland IPC */
char *hypr_request(const char *cmd, char **err);
int hypr_detect_lua(void);
int hypr_compositor_pid(pid_t *out_pid);
char *hypr_close_window(const char *address, int use_lua);
int hypr_exit(int use_lua);
void hypr_notify_error(const char *message);

/* session: collect, close and wait for apps */
int session_collect(struct Session *s);
void session_print(const struct Session *s);
int session_close(struct Session *s, unsigned timeout_sec);
void session_free(struct Session *s);

/* power / session actions */
int run_action(enum Action action, const char *command, const struct Config *cfg);

/* process helpers */
int proc_info(pid_t pid, struct ProcInfo *out);
int proc_collect_children(pid_t parent, pid_t *out, size_t cap);

#endif
