#include "hyprdown.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#endif

static int run_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "hyprdown: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "hyprdown: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "hyprdown: waitpid: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

static int run_shell(const char *command) {
    char *argv[] = {"sh", "-c", (char *)command, NULL};
    return run_argv(argv);
}

#ifdef HAVE_LIBSYSTEMD
static int logind_bool(const char *member, int interactive) {
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r;

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "hyprdown: sd_bus_open_system: %s\n", strerror(-r));
        return r;
    }

    r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                           "org.freedesktop.login1.Manager", member, &error, NULL, "b",
                           interactive);
    if (r < 0) {
        fprintf(stderr, "hyprdown: logind %s: %s\n", member,
                error.message ? error.message : strerror(-r));
    }

    sd_bus_error_free(&error);
    sd_bus_unref(bus);
    return r;
}

static int logind_terminate_session(void) {
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *session = getenv("XDG_SESSION_ID");
    int r;

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "hyprdown: sd_bus_open_system: %s\n", strerror(-r));
        return r;
    }

    if (session && session[0] != '\0') {
        r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "TerminateSession", &error,
                               NULL, "s", session);
        if (r >= 0)
            goto done;
        fprintf(stderr, "hyprdown: TerminateSession(%s): %s\n", session,
                error.message ? error.message : strerror(-r));
        sd_bus_error_free(&error);
        error = SD_BUS_ERROR_NULL;
    }

    r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                           "org.freedesktop.login1.Manager", "GetSessionByPID", &error,
                           &reply, "u", (uint32_t)getpid());
    if (r < 0) {
        fprintf(stderr, "hyprdown: GetSessionByPID: %s\n",
                error.message ? error.message : strerror(-r));
        goto done;
    }

    const char *path = NULL;
    r = sd_bus_message_read(reply, "o", &path);
    if (r < 0 || !path) {
        fprintf(stderr, "hyprdown: failed to read session object path\n");
        goto done;
    }

    r = sd_bus_call_method(bus, "org.freedesktop.login1", path,
                           "org.freedesktop.login1.Session", "Terminate", &error, NULL,
                           NULL);
    if (r < 0) {
        fprintf(stderr, "hyprdown: Session.Terminate: %s\n",
                error.message ? error.message : strerror(-r));
    }

done:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return r;
}
#endif

static int fallback_logout(void) {
    const char *session = getenv("XDG_SESSION_ID");
    if (session && session[0] != '\0') {
        char *argv[] = {"loginctl", "terminate-session", (char *)session, NULL};
        return run_argv(argv);
    }

    char uid[32];
    snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    char *argv[] = {"loginctl", "terminate-user", uid, NULL};
    return run_argv(argv);
}

static int run_override(const char *what, const char *command) {
    fprintf(stderr, "hyprdown: %s via override: %s\n", what, command);
    return run_shell(command);
}

int run_action(enum Action action, const char *command, const struct Config *cfg) {
    switch (action) {
    case ACTION_NONE:
        return 0;
    case ACTION_COMMAND:
        if (!command || command[0] == '\0') {
            fprintf(stderr, "hyprdown: empty command\n");
            return -1;
        }
        fprintf(stderr, "hyprdown: running command: %s\n", command);
        return run_shell(command);
    case ACTION_SHUTDOWN:
        if (cfg && cfg->poweroff_override && cfg->poweroff_override[0] != '\0')
            return run_override("shutting down", cfg->poweroff_override);
        fprintf(stderr, "hyprdown: shutting down\n");
#ifdef HAVE_LIBSYSTEMD
        if (logind_bool("PowerOff", 0) >= 0)
            return 0;
        fprintf(stderr, "hyprdown: falling back to systemctl poweroff\n");
#endif
        {
            char *argv[] = {"systemctl", "poweroff", NULL};
            return run_argv(argv);
        }
    case ACTION_REBOOT:
        if (cfg && cfg->reboot_override && cfg->reboot_override[0] != '\0')
            return run_override("rebooting", cfg->reboot_override);
        fprintf(stderr, "hyprdown: rebooting\n");
#ifdef HAVE_LIBSYSTEMD
        if (logind_bool("Reboot", 0) >= 0)
            return 0;
        fprintf(stderr, "hyprdown: falling back to systemctl reboot\n");
#endif
        {
            char *argv[] = {"systemctl", "reboot", NULL};
            return run_argv(argv);
        }
    case ACTION_LOGOUT:
        if (cfg && cfg->logout_override && cfg->logout_override[0] != '\0')
            return run_override("logging out", cfg->logout_override);
        fprintf(stderr, "hyprdown: terminating session\n");
#ifdef HAVE_LIBSYSTEMD
        if (logind_terminate_session() >= 0)
            return 0;
        fprintf(stderr, "hyprdown: falling back to loginctl\n");
#endif
        return fallback_logout();
    }

    return -1;
}
