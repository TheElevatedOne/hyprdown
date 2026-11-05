#!/usr/bin/env python3
"""End-to-end tests against tests/fake_hyprland.py. Usage: run_tests.py BINARY

Refuses to run outside GitHub Actions: hyprdown powers machines off, and these
tests are not worth the risk on a machine somebody is using."""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

if os.environ.get("GITHUB_ACTIONS") != "true":
    sys.exit("refusing to run hyprdown outside GitHub Actions")

BINARY = os.path.abspath(sys.argv[1])
FAKE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_hyprland.py")
SAFE_CONFIG = """
poweroff_override = "echo poweroff > {out}/action"
reboot_override = "echo reboot > {out}/action"
logout_override = "echo logout > {out}/action"
"""
APPS = {
    "windows": [
        {"name": "normal", "kind": "normal", "title": "x" * 70000},
        {"name": "tray", "kind": "tray"},
        {"name": "stubborn", "kind": "stubborn", "mode": "ignore-term"},
        {"name": "respawn", "kind": "respawn"},
    ],
    "layers": ["bar"],
    "children": [{"name": "daemon"}, {"name": "Xwayland"}],
}


class Env:
    def __init__(self, scenario, config=None):
        self.dir = tempfile.mkdtemp(prefix="hyprdown-test-")
        self.out = self.dir
        for sub in ("config/hyprdown", "state", "run"):
            os.makedirs(os.path.join(self.dir, sub))
        with open(os.path.join(self.dir, "config/hyprdown/config.toml"), "w") as f:
            f.write((SAFE_CONFIG if config is None else config).format(out=self.out))
        self.fake = subprocess.Popen(
            [sys.executable, "-I", FAKE, os.path.join(self.dir, "run"), self.out, json.dumps(scenario)],
            start_new_session=True)
        wait_for(lambda: os.path.exists(self.path("fake.ready")), 10, "fake compositor start")
        self.env = dict(os.environ, HYPRLAND_INSTANCE_SIGNATURE="test",
                        XDG_RUNTIME_DIR=os.path.join(self.dir, "run"),
                        XDG_CONFIG_HOME=os.path.join(self.dir, "config"),
                        XDG_STATE_HOME=os.path.join(self.dir, "state"))

    def path(self, name):
        return os.path.join(self.dir, name)

    def run(self, *args, **kwargs):
        return subprocess.run([BINARY, *args], env=self.env, timeout=30, **kwargs)

    def events(self):
        try:
            with open(self.path("events")) as f:
                return [line.split(" ", 1) for line in f.read().splitlines()]
        except FileNotFoundError:
            return []

    def times(self, text):
        return [float(t) for t, e in self.events() if e == text]

    def pid(self, name):
        with open(self.path(f"{name}.pid")) as f:
            return int(f.read())

    def log(self):
        with open(self.path("state/hyprdown/hyprdown.log")) as f:
            return f.read()

    def cleanup(self):
        try:
            os.killpg(self.fake.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.fake.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


def wait_for(check, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def dead(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rsplit(")", 1)[1].split()[0] in "ZX"
    except FileNotFoundError:
        return True


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def test_dry_run_changes_nothing():
    env = Env(APPS)
    try:
        out = env.run("-n", "-s", stdout=subprocess.PIPE, text=True, check=True).stdout
        for name in ("normal", "tray", "stubborn", "bar", "daemon"):
            require(name in out, f"{name} missing from dry run: {out[:300]}")
        require("Xwayland" not in out, "Xwayland listed for closing")
        require(not env.times("close normal"), "dry run closed a window")
        require(not dead(env.pid("daemon")), "dry run killed a process")
        require(not os.path.exists(env.path("action")), "dry run ran the action")
    finally:
        env.cleanup()


def test_full_shutdown_from_script_menu():
    """Same shape as rofi script mode: read stdout until EOF, then carry on."""
    env = Env(APPS)
    try:
        start = time.monotonic()
        proc = subprocess.Popen([BINARY, "-t", "3", "-c", f"grep -E 'SigIgn' /proc/self/status > {env.out}/action"],
                                env=env.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        proc.communicate(timeout=10)
        require(time.monotonic() - start < 1, "launcher blocked: stdio kept open by the detached process")
        require(proc.returncode == 0, f"launcher exit {proc.returncode}")

        wait_for(lambda: os.path.exists(env.path("action")), 15, "action")
        for name in ("normal", "tray", "stubborn", "respawn", "respawn-new", "bar", "daemon"):
            require(dead(env.pid(name)), f"{name} still alive")
        require(not dead(env.pid("Xwayland")), "Xwayland was signalled")
        require(env.fake.poll() is None, "compositor was signalled")
        require(env.times("close respawn-new"), "window opened during shutdown was not closed")
        require(not env.times("exit"), "Hyprland exited although an action was given")

        require(env.times("term tray")[0] - env.times("close tray")[0] >= 0.9, "tray app did not get its grace period")
        require(len(env.times("term tray")) == 1, "SIGTERM sent more than once")
        require(not env.times("term normal"), "normal app was sent SIGTERM while closing")
        require(env.times("term stubborn"), "stubborn app never got SIGTERM")

        with open(env.path("action")) as f:
            ignored = int(f.read().split()[1], 16)
        for sig in (signal.SIGHUP, signal.SIGPIPE, signal.SIGTERM):
            require(not ignored & (1 << (sig - 1)), f"action inherited ignored {sig.name}")
        require("hyprdown" in env.log() and "timeout" in env.log(), f"log incomplete: {env.log()}")
    finally:
        env.cleanup()


def test_builtin_actions_use_overrides():
    for flag, word in (("-s", "poweroff"), ("-r", "reboot"), ("-l", "logout")):
        env = Env({"windows": [{"name": "normal", "kind": "normal"}]})
        try:
            env.run(flag, "-t", "2", check=True)
            wait_for(lambda: os.path.exists(env.path("action")), 10, f"{flag} action")
            with open(env.path("action")) as f:
                require(f.read().strip() == word, f"{flag} ran the wrong action")
        finally:
            env.cleanup()


def test_no_action_exits_hyprland():
    env = Env({"windows": [{"name": "normal", "kind": "normal"}]})
    try:
        env.run("-t", "2", check=True)
        wait_for(lambda: env.times("exit"), 10, "exit dispatch")
        require(env.times("close normal")[0] < env.times("exit")[0], "exited before closing windows")
    finally:
        env.cleanup()


def test_failed_action_notifies():
    env = Env({"windows": [{"name": "normal", "kind": "normal"}]})
    try:
        env.run("-t", "2", "-c", "exit 3", check=True)
        wait_for(lambda: any("/notify" in e for _, e in env.events()), 10, "notification")
    finally:
        env.cleanup()


def test_invalid_config_closes_nothing():
    env = Env({"windows": [{"name": "normal", "kind": "normal"}]}, config="bad_key = 1\n")
    try:
        result = env.run("-s", stderr=subprocess.DEVNULL)
        require(result.returncode == 1, f"exit {result.returncode}")
        require(not env.times("close normal"), "closed windows with a broken config")
        require(any("/notify" in e for _, e in env.events()), "no notification")
    finally:
        env.cleanup()


def test_rejected_close_is_logged_once():
    env = Env({"windows": [{"name": "rejected", "kind": "reject"}]})
    try:
        env.run("-t", "3", "-c", f"touch {env.out}/action", check=True)
        wait_for(lambda: os.path.exists(env.path("action")), 15, "action")
        require(env.log().count("rejected by test") == 1, f"expected one warning: {env.log()}")
        require(dead(env.pid("rejected")), "process survived the timeout")
    finally:
        env.cleanup()


def test_survives_sigterm():
    """A launcher running as a systemd service gets its whole cgroup signalled."""
    env = Env({"windows": [{"name": "stubborn", "kind": "stubborn", "mode": "ignore-term"}]})
    try:
        proc = subprocess.Popen([BINARY, "--no-fork", "-t", "2", "-c", f"touch {env.out}/action"], env=env.env)
        wait_for(lambda: env.times("close stubborn"), 5, "first close")
        proc.send_signal(signal.SIGTERM)
        require(proc.wait(timeout=15) == 0, "hyprdown died on SIGTERM")
        require(os.path.exists(env.path("action")), "action did not run")
    finally:
        env.cleanup()


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"ok   {test.__name__}", flush=True)
        except Exception as error:  # pylint: disable=broad-except
            failed += 1
            print(f"FAIL {test.__name__}: {error!r}"[:2000], flush=True)
    print(f"{len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)


main()
