#include "hyprdown.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct option long_opts[] = {
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'V'},
    {"shutdown", no_argument, NULL, 's'},
    {"reboot", no_argument, NULL, 'r'},
    {"logout", no_argument, NULL, 'l'},
    {"timeout", required_argument, NULL, 't'},
    {"command", required_argument, NULL, 'c'},
    {0, 0, 0, 0},
};

void print_help(unsigned default_timeout) {
  printf(
      "Usage: hyprdown [OPTIONS]\n"
      "\n"
      "Gracefully close Hyprland clients. With no action, Hyprland is then\n"
      "exited. With an action (-s, -r, -l, -c), that action runs after\n"
      "clients close; Hyprland is not exited first.\n"
      "\n"
      "Clients that do not exit are waited on for a limited time\n"
      "(default: %u seconds) before remaining apps are killed.\n"
      "\n"
      "Options:\n"
      "  --help              Show this help and exit\n"
      "  --version           Show version and exit\n"
      "  -s, --shutdown      Power off\n"
      "  -r, --reboot        Reboot\n"
      "  -l, --logout        Terminate the session (return to the display "
      "manager)\n"
      "  -t, --timeout SECS  Seconds to wait for apps to close (default: %u)\n"
      "  -c, --command CMD   Command to run after apps close\n"
      "\n"
      "Actions are mutually exclusive. With no action, Hyprland is exited\n"
      "after apps close.\n"
      "\n"
      "Configuration is read from:\n"
      "  $XDG_CONFIG_HOME/hyprdown/config.toml\n"
      "  or $HOME/.config/hyprdown/config.toml\n"
      "\n"
      "  timeout             Default wait in seconds (overridden by -t)\n"
      "  poweroff_override   Command run instead of builtin poweroff (-s)\n"
      "  reboot_override     Command run instead of builtin reboot (-r)\n"
      "  logout_override     Command run instead of builtin logout (-l)\n"
      "\n"
      "The config directory and an empty config.toml are created on first "
      "run.\n"
      "\n"
      "Power and session actions use systemd-logind over D-Bus when "
      "available,\n"
      "and otherwise fall back to systemctl/loginctl, unless an override is "
      "set.\n",
      default_timeout, default_timeout);
}

void print_version(void) { printf("hyprdown %s\n", HYPRDOWN_VERSION); }

static int set_action(struct Options *opt, enum Action action,
                      const char *flag) {
  if (opt->action != ACTION_NONE) {
    fprintf(stderr, "hyprdown: %s conflicts with another action\n", flag);
    return -1;
  }
  opt->action = action;
  return 0;
}

int parse_args(int argc, char **argv, struct Options *opt,
               unsigned default_timeout) {
  memset(opt, 0, sizeof(*opt));
  opt->timeout_sec = default_timeout;

  opterr = 0;
  int c;
  while ((c = getopt_long(argc, argv, ":srlt:c:hV", long_opts, NULL)) != -1) {
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
    case 't': {
      char *end = NULL;
      errno = 0;
      long v = strtol(optarg, &end, 10);
      if (errno != 0 || !end || *end != '\0' || v < 0 || v > INT_MAX) {
        fprintf(stderr, "hyprdown: invalid timeout '%s'\n", optarg);
        return -1;
      }
      opt->timeout_sec = (unsigned)v;
      break;
    }
    case 'c':
      if (set_action(opt, ACTION_COMMAND, "--command") < 0)
        return -1;
      opt->command = optarg;
      if (!opt->command || opt->command[0] == '\0') {
        fprintf(stderr, "hyprdown: --command requires a non-empty command\n");
        return -1;
      }
      break;
    case ':':
      fprintf(stderr, "hyprdown: option -%c requires an argument\n", optopt);
      return -1;
    case '?':
    default:
      if (optopt)
        fprintf(stderr, "hyprdown: unknown option -%c\n", optopt);
      else
        fprintf(stderr, "hyprdown: unknown option %s\n",
                argv[optind - 1] ? argv[optind - 1] : "");
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

static void ignore_hangup(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGPIPE, &sa, NULL);
}

static int hypr_exit(int use_lua) {
  const char *cmd = use_lua ? "/dispatch hl.dsp.exit()" : "/dispatch exit";
  char *err = NULL;
  char *reply = hypr_request(cmd, &err);

  if (!reply) {
    fprintf(stderr, "hyprdown: failed to exit Hyprland: %s\n",
            err ? err : "IPC error");
    free(err);
    return -1;
  }

  free(err);
  free(reply);
  return 0;
}

int main(int argc, char **argv) {
  struct Config cfg;
  struct Options opt;
  struct Hyprland hl = {0};
  struct AppList apps = {0};
  unsigned default_timeout = HYPRDOWN_DEFAULT_TIMEOUT;
  int cfg_rc;
  int rc = 0;

  cfg_rc = config_init(&cfg);
  if (cfg.has_timeout)
    default_timeout = cfg.timeout_sec;

  if (parse_args(argc, argv, &opt, default_timeout) < 0) {
    config_free(&cfg);
    return 2;
  }

  if (cfg_rc < 0) {
    config_free(&cfg);
    return 1;
  }

  if (!getenv("HYPRLAND_INSTANCE_SIGNATURE") ||
      getenv("HYPRLAND_INSTANCE_SIGNATURE")[0] == '\0') {
    fprintf(stderr, "hyprdown: HYPRLAND_INSTANCE_SIGNATURE is unset; "
                    "this tool only works under Hyprland\n");
    config_free(&cfg);
    return 1;
  }

  ignore_hangup();

  hl.use_lua = hypr_detect_lua();
  if (hypr_compositor_pid(&hl.compositor_pid) < 0)
    hl.compositor_pid = -1;

  if (apps_collect(&apps, &hl) < 0) {
    apps_free(&apps);
    config_free(&cfg);
    return 1;
  }

  fprintf(stderr, "hyprdown: closing %zu client(s)%s\n", apps.count,
          hl.use_lua ? " (lua dispatchers)" : "");

  apps_quit(&apps, hl.use_lua);

  if (apps.count > 0) {
    int wait_rc = apps_wait(&apps, opt.timeout_sec, hl.use_lua);
    if (wait_rc < 0) {
      apps_free(&apps);
      config_free(&cfg);
      return 1;
    }
  }

  apps_free(&apps);

  if (opt.action == ACTION_NONE) {
    fprintf(stderr, "hyprdown: exiting Hyprland\n");
    if (hypr_exit(hl.use_lua) < 0)
      rc = 1;
  } else if (run_action(opt.action, opt.command, &cfg) != 0) {
    rc = 1;
  }

  config_free(&cfg);
  return rc;
}
