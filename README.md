# hyprdown

A CLI counterpart to [hyprshutdown](https://github.com/hyprwm/hyprshutdown) for Hyprland.

![Static Badge](https://img.shields.io/badge/Vibecoded-Grok-black?style=for-the-badge&labelColor=%236F0E82)

It asks every client to close and waits a limited time (5 seconds by default). Apps that ignore the close request are not waited on forever. With no action, Hyprland is then exited. With `-s`, `-r`, `-l`, or `-c`, that action runs after clients close — Hyprland is not exited first.

## Build

Needs `gcc`, `pkg-config`, `json-c`, and (optionally) `libsystemd` for logind D-Bus calls.

```
make
sudo make install
```

`make` writes objects to `build/`, the binary to `dist/hyprdown`, and a release archive to `dist/hyprdown-<version>-linux.tar.gz`.

If `libsystemd` is missing, power and logout actions fall back to `systemctl` / `loginctl`.

## Usage

```
hyprdown                  # close apps, then exit Hyprland
hyprdown -s               # close apps, then power off
hyprdown -r               # close apps, then reboot
hyprdown -l               # close apps, then terminate the session (display manager)
hyprdown -t 10 -s         # wait up to 10s for apps, then power off
hyprdown -c 'shutdown -P 0'   # close apps, then run a command
```

```
--help              Show help
--version           Show version
-s, --shutdown      Power off
-r, --reboot        Reboot
-l, --logout        Terminate the login session
-t, --timeout SECS  Wait this many seconds for apps (default: 5, or config)
-c, --command CMD   Run CMD after apps close
```

Must be run under Hyprland (`HYPRLAND_INSTANCE_SIGNATURE` set), except `--help` / `--version`.

## Configuration

On first run (including `--help`), hyprdown creates:

```
$XDG_CONFIG_HOME/hyprdown/config.toml
```

or, if `XDG_CONFIG_HOME` is unset:

```
$HOME/.config/hyprdown/config.toml
```

The file is created empty. Optional keys:

```toml
timeout = 10
poweroff_override = "systemctl poweroff"
reboot_override = "systemctl reboot"
logout_override = "loginctl terminate-session $XDG_SESSION_ID"
```

- `timeout` — default wait in seconds when `-t` is not given
- `poweroff_override` — command to run instead of builtin poweroff when using `-s`
- `reboot_override` — command to run instead of builtin reboot when using `-r`
- `logout_override` — command to run instead of builtin logout when using `-l`

`-t` always overrides the config timeout. Empty override strings are ignored.
