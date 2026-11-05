#include "hyprdown.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct option long_opts[] = {
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'V'},
    {"shutdown", no_argument, NULL, 's'},
    {"reboot", no_argument, NULL, 'r'},
    {"logout", no_argument, NULL, 'l'},
    {"timeout", required_argument, NULL, 't'},
    {"command", required_argument, NULL, 'c'},
    {"dry-run", no_argument, NULL, 'n'},
    {"no-fork", no_argument, NULL, 'F'},
    {0, 0, 0, 0},
};

void print_help(unsigned default_timeout) {
    printf("Usage: hyprdown [OPTIONS]\n"
           "\n"
           "Gracefully close every Hyprland app, then exit Hyprland or run an\n"
           "action. Windows are asked to close through the compositor; processes\n"
           "without windows left (tray apps, layer-shell clients, exec-once\n"
           "daemons) get SIGTERM. Whatever is still running after the timeout is\n"
           "killed with SIGKILL.\n"
           "\n"
           "Options:\n"
           "  -h, --help          Show this help and exit\n"
           "  -V, --version       Show version and exit\n"
           "  -s, --shutdown      Power off\n"
           "  -r, --reboot        Reboot\n"
           "  -l, --logout        Terminate the login session\n"
           "  -c, --command CMD   Run CMD (via sh -c) after apps close\n"
           "  -t, --timeout SECS  Seconds to wait for apps (default: %u)\n"
           "  -n, --dry-run       List what would be closed; change nothing\n"
           "      --no-fork       Stay in the foreground and log to stderr\n"
           "\n"
           "Actions are mutually exclusive. With no action, Hyprland is exited\n"
           "after apps close. With an action, Hyprland is left running and the\n"
           "action takes care of it.\n"
           "\n"
           "hyprdown detaches and returns right away, so failures do not show up\n"
           "in its exit status. They are written to\n"
           "$XDG_STATE_HOME/hyprdown/hyprdown.log (or\n"
           "$HOME/.local/state/hyprdown/hyprdown.log) and shown as a Hyprland\n"
           "notification.\n"
           "\n"
           "Configuration ($XDG_CONFIG_HOME/hyprdown/config.toml or\n"
           "$HOME/.config/hyprdown/config.toml, created empty on first run):\n"
           "  timeout             Default wait in seconds (overridden by -t)\n"
           "  poweroff_override   Command run instead of builtin poweroff (-s)\n"
           "  reboot_override     Command run instead of builtin reboot (-r)\n"
           "  logout_override     Command run instead of builtin logout (-l)\n"
           "\n"
           "Power and session actions use systemd-logind over D-Bus when\n"
           "available and fall back to systemctl/loginctl.\n",
           default_timeout);
}

void print_version(void) { printf("hyprdown %s\n", HYPRDOWN_VERSION); }

static int set_action(struct Options *opt, enum Action action, const char *flag) {
    if (opt->action != ACTION_NONE) {
        fprintf(stderr, "hyprdown: %s conflicts with another action\n", flag);
        return -1;
    }
    opt->action = action;
    return 0;
}

int parse_args(int argc, char **argv, struct Options *opt, unsigned default_timeout) {
    memset(opt, 0, sizeof(*opt));
    opt->timeout_sec = default_timeout;

    opterr = 0;
    int c;
    while ((c = getopt_long(argc, argv, ":hVsrlt:c:n", long_opts, NULL)) != -1) {
        switch (c) {
        case 'h':
            print_help(default_timeout);
            exit(0);
        case 'V':
            print_version();
            exit(0);
        case 's':
            if (set_action(opt, ACTION_SHUTDOWN, "--shutdown") < 0)
                return -1;
            break;
        case 'r':
            if (set_action(opt, ACTION_REBOOT, "--reboot") < 0)
                return -1;
            break;
        case 'l':
            if (set_action(opt, ACTION_LOGOUT, "--logout") < 0)
                return -1;
            break;
        case 'c':
            if (set_action(opt, ACTION_COMMAND, "--command") < 0)
                return -1;
            if (optarg[0] == '\0') {
                fprintf(stderr, "hyprdown: --command requires a non-empty command\n");
                return -1;
            }
            opt->command = optarg;
            break;
        case 't': {
            char *end = NULL;
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' || v < 0 || v > INT_MAX) {
                fprintf(stderr, "hyprdown: invalid timeout '%s'\n", optarg);
                return -1;
            }
            opt->timeout_sec = (unsigned)v;
            break;
        }
        case 'n':
            opt->dry_run = 1;
            break;
        case 'F':
            opt->no_fork = 1;
            break;
        case ':':
            fprintf(stderr, "hyprdown: option '%s' requires an argument\n", argv[optind - 1]);
            return -1;
        default:
            fprintf(stderr, "hyprdown: unknown option '%s'\n", argv[optind - 1]);
            fprintf(stderr, "Try 'hyprdown --help' for more information.\n");
            return -1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "hyprdown: unexpected argument '%s'\n", argv[optind]);
        fprintf(stderr, "Try 'hyprdown --help' for more information.\n");
        return -1;
    }

    return 0;
}

static void ignore_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    /* Whatever started us is likely one of the apps being closed. Losing it,
     * or systemd stopping the unit it runs in, must not stop the shutdown
     * halfway. SIGINT still cancels a foreground run. */
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* Detach from whatever launched us. The parent returns immediately, and no
 * stdio is kept open: a rofi script-mode menu reads our stdout until EOF and
 * would otherwise stay on screen, blocked, until the shutdown is over. */
static int detach(void) {
    char *log_path = NULL;
    int log_fd = log_open(&log_path);
    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

    if (log_fd < 0)
        fprintf(stderr, "hyprdown: warning: cannot open log file\n");
    else if (isatty(STDERR_FILENO))
        fprintf(stderr, "hyprdown: continuing in the background, log: %s\n", log_path);
    free(log_path);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "hyprdown: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0)
        _exit(0);

    if (setsid() < 0)
        fprintf(stderr, "hyprdown: setsid: %s\n", strerror(errno));

    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
    }
    if (log_fd >= 0)
        dup2(log_fd, STDERR_FILENO);
    else if (null_fd >= 0)
        dup2(null_fd, STDERR_FILENO);
    return 0;
}

static void log_start(const struct Options *opt) {
    static const char *const names[] = {
        [ACTION_NONE] = "exit Hyprland",
        [ACTION_SHUTDOWN] = "power off",
        [ACTION_REBOOT] = "reboot",
        [ACTION_LOGOUT] = "log out",
        [ACTION_COMMAND] = "run command",
    };
    char when[64] = "";
    time_t now = time(NULL);
    struct tm tm;

    if (localtime_r(&now, &tm))
        strftime(when, sizeof(when), "%F %T", &tm);
    fprintf(stderr, "hyprdown %s: %s: %s, timeout %us\n", HYPRDOWN_VERSION, when,
            names[opt->action], opt->timeout_sec);
}

/* In the foreground our callers (e.g. `sh -c "hyprdown && ..."`) may be
 * compositor children; they must not be terminated under us. */
static void protect_ancestors(struct Session *s) {
    pid_t pid = getppid();
    struct ProcInfo info;

    while (pid > 1 && pid != s->compositor_pid && s->nprotected < HYPRDOWN_MAX_PROTECTED) {
        s->protected_pids[s->nprotected++] = pid;
        if (proc_info(pid, &info) < 0)
            break;
        pid = info.ppid;
    }
}

int main(int argc, char **argv) {
    struct Config cfg;
    struct Options opt;
    struct Session session;
    unsigned default_timeout = HYPRDOWN_DEFAULT_TIMEOUT;
    int rc = 0;

    int cfg_rc = config_init(&cfg);
    if (cfg.has_timeout)
        default_timeout = cfg.timeout_sec;

    if (parse_args(argc, argv, &opt, default_timeout) < 0) {
        config_free(&cfg);
        return 2;
    }
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!his || his[0] == '\0') {
        fprintf(stderr, "hyprdown: HYPRLAND_INSTANCE_SIGNATURE is unset; "
                        "this tool only works under Hyprland\n");
        config_free(&cfg);
        return 1;
    }

    /* Refuse before closing anything: overrides may be what makes the action work. */
    if (cfg_rc < 0) {
        hypr_notify_error("invalid config.toml, nothing was closed");
        config_free(&cfg);
        return 1;
    }

    memset(&session, 0, sizeof(session));
    session.compositor_pid = -1;
    if (hypr_compositor_pid(&session.compositor_pid) < 0)
        session.compositor_pid = -1;

    if (!opt.dry_run) {
        ignore_signals();
        if (opt.no_fork)
            protect_ancestors(&session);
        else if (detach() < 0) {
            config_free(&cfg);
            return 1;
        }
        log_start(&opt);
    }

    session.use_lua = hypr_detect_lua();

    if (session_collect(&session) < 0) {
        hypr_notify_error("cannot list clients, nothing was closed");
        session_free(&session);
        config_free(&cfg);
        return 1;
    }

    if (opt.dry_run) {
        session_print(&session);
        session_free(&session);
        config_free(&cfg);
        return 0;
    }

    fprintf(stderr, "hyprdown: closing %zu window(s), %zu process(es) (%s)\n", session.nwindows,
            session.nprocs, session.use_lua ? "lua" : "hyprlang");

    session_close(&session, opt.timeout_sec);
    session_free(&session);

    if (opt.action == ACTION_NONE) {
        fprintf(stderr, "hyprdown: exiting Hyprland\n");
        if (hypr_exit(session.use_lua) < 0)
            rc = 1;
    } else if (run_action(opt.action, opt.command, &cfg) != 0) {
        hypr_notify_error("apps were closed, but the action failed; see hyprdown.log");
        rc = 1;
    }

    config_free(&cfg);
    return rc;
}
