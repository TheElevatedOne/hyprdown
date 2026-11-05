# hyprdown

![Static Badge](https://img.shields.io/badge/CLAUDE-VIBECODED?style=for-the-badge&logo=claude&label=VIBECODED&labelColor=%23333333&color=%23de7356) [![AUR Version](https://img.shields.io/aur/version/hyprdown-bin?style=for-the-badge&logo=archlinux)](https://aur.archlinux.org/packages/hyprdown-bin)

Closes every app running under Hyprland without asking you anything, then exits Hyprland, powers off, reboots, logs out or runs your own command. It's a CLI counterpart to [hyprshutdown](https://github.com/hyprwm/hyprshutdown), with no UI.

```sh
hyprdown -s    # close everything, then power off
```

## How it works

1. **Collect**: client windows (`j/clients`), layer-shell clients such as bars and wallpapers (`j/layers`), and direct children of the compositor.
2. **Close windows**: each window is asked to close through `hl.dsp.window.close`, and again every second while it stays open. Windows that appear during the shutdown are closed too.
3. **SIGTERM**: a process gets one SIGTERM once it has no windows left. An app that had windows gets 1 second to exit by itself first.
4. **SIGKILL**: whatever is still running after the timeout (5 seconds by default) is killed.
5. **Action**: Hyprland is exited, or the action you asked for runs. With an action, Hyprland is not exited first.

**Never signalled**: Hyprland, Xwayland, `start-hyprland`, `uwsm`, hyprdown itself, pid 1 and other users' processes. Processes are tracked by pid and start time, so a reused pid is never hit.

## Requirements

- Hyprland 0.55 or newer with a Lua config. Older versions use the legacy dispatchers. Non-Lua configs on current Hyprland are not supported, because `dispatch` only works with Lua there.
- `json-c` and `libsystemd`. Without `libsystemd` at build time, power and logout actions call `systemctl` and `loginctl`.

## Install

| Package | Install | Builds from |
| --- | --- | --- |
| [`hyprdown-bin`](https://aur.archlinux.org/packages/hyprdown-bin) | `paru -S hyprdown-bin` | Release binary |
| `hyprdown-git` | `makepkg -si` in [`aur/hyprdown-git`](aur/hyprdown-git) (not on the AUR yet) | `main` branch |

### Release tarball

Download `hyprdown-<version>-linux.tar.gz` from the [releases page](https://github.com/TheElevatedOne/hyprdown/releases). It holds the `hyprdown` binary.

### Building

Needs `gcc`, `make`, `pkg-config` and the `json-c` headers. The `libsystemd` headers are optional.

```sh
make
sudo make install
```

`make` writes the binary to `dist/hyprdown` and a release archive to `dist/hyprdown-<version>-linux.tar.gz`. Set `PREFIX` and `DESTDIR` to change the install location.

## Usage

> [!WARNING]
> Any command except `-n`, `--help` and `--version` closes all your apps. Use `hyprdown -n` to see what would happen first.

```sh
hyprdown -n                  # dry run: list what would be closed, change nothing
hyprdown                     # close apps, then exit Hyprland
hyprdown -s                  # close apps, then power off
hyprdown -r                  # close apps, then reboot
hyprdown -l                  # close apps, then terminate the login session
hyprdown -t 10 -s            # wait up to 10 seconds for apps, then power off
hyprdown -c 'my-script.sh'   # close apps, then run a command
```

| Option | Description |
| --- | --- |
| `-h`, `--help` | Show help |
| `-V`, `--version` | Show version |
| `-s`, `--shutdown` | Power off |
| `-r`, `--reboot` | Reboot |
| `-l`, `--logout` | Terminate the login session |
| `-c`, `--command CMD` | Run `CMD` through `sh -c` after apps close |
| `-t`, `--timeout SECS` | Seconds to wait for apps. Default 5, or `timeout` from the config. `0` kills what is left right away |
| `-n`, `--dry-run` | List windows and processes that would be closed, then exit |
| `--no-fork` | Stay in the foreground and log to stderr |

Actions are mutually exclusive. hyprdown has to run under Hyprland (`HYPRLAND_INSTANCE_SIGNATURE` set), except for `--help` and `--version`.

### Keybinds and menus

```lua
hl.bind("SUPER + M", hl.dsp.exec_cmd("hyprdown"))
hl.bind("SUPER + SHIFT + M", hl.dsp.exec_cmd("hyprdown -s"))
```

hyprdown detaches and returns right away, and keeps no stdio open. This means it works from a [rofi](https://github.com/davatorium/rofi) script-mode menu, which waits for its script's output to end. It also means the exit status only covers argument and config errors, so `hyprdown -s && something` doesn't wait for the shutdown. Use `--no-fork` for that.

## Configuration

hyprdown creates an empty config file on first run, at `$XDG_CONFIG_HOME/hyprdown/config.toml`, or `$HOME/.config/hyprdown/config.toml` if `XDG_CONFIG_HOME` is unset.

| Key | Default | Description |
| --- | --- | --- |
| `timeout` | `5` | Seconds to wait for apps. `-t` overrides it |
| `poweroff_override` | none | Command to run instead of the builtin power off (`-s`) |
| `reboot_override` | none | Command to run instead of the builtin reboot (`-r`) |
| `logout_override` | none | Command to run instead of the builtin logout (`-l`) |

```toml
timeout = 10
poweroff_override = "systemctl poweroff"
reboot_override = "systemctl reboot"
logout_override = "loginctl terminate-session $XDG_SESSION_ID"
```

Empty override strings are ignored. If the config is invalid, hyprdown shows a Hyprland notification and closes nothing.

## Troubleshooting

A detached run logs to `$XDG_STATE_HOME/hyprdown/hyprdown.log`, or `$HOME/.local/state/hyprdown/hyprdown.log`. The file is replaced on every run and survives a reboot. If the action fails after apps were closed, you also get a Hyprland notification.

Please include that log when you [report a bug](https://github.com/TheElevatedOne/hyprdown/issues).

## Tests

The tests run hyprdown, so they only run on GitHub Actions and refuse to start anywhere else. See [`.github/workflows/test.yml`](.github/workflows/test.yml).

## License

[BSD-3-Clause](LICENSE)
